((functions_block ()) (input_vars ((B <opaque> SInt)))
 (prepare_data
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id B) (decl_type (Sized SInt))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable B) ()) UInt
      ((pattern
        (Indexed
         ((pattern
           (FunApp (CompilerInternal FnReadData)
            (((pattern (Lit Str B))
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
        (var_name B)
        (var ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (((pattern (Lit Int 0)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnCheck
        (trans
         (Upper
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
        (var_name B)
        (var ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str simp)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str corr)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str cov)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str chol_corr))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str chol_sq))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (StanLib check_greater_or_equal FnPlain AoS)
      (((pattern (Lit Str "cholesky_factor_cov chol_sq"))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str "num rows (must be greater or equal to num cols)"))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str chol_rect))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (StanLib check_greater_or_equal FnPlain AoS)
      (((pattern (Lit Str "cholesky_factor_cov chol_rect"))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str "num rows (must be greater or equal to num cols)"))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str stz_vec))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str stz_mat))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str stz_row_zero))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSizePositive)
      (((pattern (Lit Str stz_row_zero))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSize)
      (((pattern (Lit Str stz_col_zero))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp (CompilerInternal FnValidateSizePositive)
      (((pattern (Lit Str stz_col_zero))
        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Str 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
    (meta <opaque>))))
 (log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id simp)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Simplex)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id corr)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Correlation)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id cov)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Covariance)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id chol_corr)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain CholeskyCorr)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id chol_sq)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain CholeskyCov)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id chol_rect)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain CholeskyCov)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id stz_vec)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain SumToZero)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id stz_mat)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain SumToZero)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id stz_row_zero)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain SumToZero)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id stz_col_zero)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain SumToZero)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id anchor) (decl_type (Sized SReal))
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
         (For (loopvar b)
          (lower
           ((pattern (Lit Int 1))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (upper
           ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (body
           ((pattern
             (Block
              (((pattern
                 (TargetPE
                  ((pattern
                    (FunApp (StanLib Times__ FnPlain AoS)
                     (((pattern
                        (Promotion
                         ((pattern (Var b))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
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
                                            (FunApp (StanLib Plus__ FnPlain AoS)
                                             (((pattern
                                                (FunApp (StanLib Plus__ FnPlain AoS)
                                                 (((pattern
                                                    (FunApp (StanLib Plus__ FnPlain AoS)
                                                     (((pattern
                                                        (FunApp
                                                         (StanLib Plus__ FnPlain AoS)
                                                         (((pattern
                                                            (FunApp
                                                             (StanLib Plus__ FnPlain AoS)
                                                             (((pattern
                                                                (FunApp
                                                                 (StanLib Plus__ FnPlain
                                                                  AoS)
                                                                 (((pattern
                                                                    (FunApp
                                                                    (StanLib Plus__
                                                                    FnPlain AoS)
                                                                    (((pattern
                                                                    (FunApp
                                                                    (StanLib Plus__
                                                                    FnPlain AoS)
                                                                    (((pattern
                                                                    (FunApp
                                                                    (StanLib Plus__
                                                                    FnPlain AoS)
                                                                    (((pattern
                                                                    (FunApp
                                                                    (StanLib Plus__
                                                                    FnPlain AoS)
                                                                    (((pattern
                                                                    (FunApp
                                                                    (StanLib sum FnPlain
                                                                    AoS)
                                                                    (((pattern
                                                                    (Indexed
                                                                    ((pattern (Var simp))
                                                                    (meta
                                                                    ((type_
                                                                    (UArray UVector))
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((Single
                                                                    ((pattern (Var b))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))))))
                                                                    (meta
                                                                    ((type_ UVector)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((pattern
                                                                    (FunApp
                                                                    (StanLib Times__
                                                                    FnPlain AoS)
                                                                    (((pattern
                                                                    (Promotion
                                                                    ((pattern
                                                                    (Lit Int 2))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))
                                                                    UReal DataOnly))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))
                                                                    ((pattern
                                                                    (Indexed
                                                                    ((pattern (Var simp))
                                                                    (meta
                                                                    ((type_
                                                                    (UArray UVector))
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((Single
                                                                    ((pattern (Var b))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly)))))
                                                                    (Single
                                                                    ((pattern
                                                                    (Lit Int 1))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((pattern
                                                                    (FunApp
                                                                    (StanLib sum FnPlain
                                                                    AoS)
                                                                    (((pattern
                                                                    (Indexed
                                                                    ((pattern (Var corr))
                                                                    (meta
                                                                    ((type_
                                                                    (UArray UMatrix))
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((Single
                                                                    ((pattern (Var b))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))))))
                                                                    (meta
                                                                    ((type_ UMatrix)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((pattern
                                                                    (FunApp
                                                                    (StanLib Times__
                                                                    FnPlain AoS)
                                                                    (((pattern
                                                                    (Promotion
                                                                    ((pattern
                                                                    (Lit Int 3))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))
                                                                    UReal DataOnly))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))
                                                                    ((pattern
                                                                    (Indexed
                                                                    ((pattern (Var corr))
                                                                    (meta
                                                                    ((type_
                                                                    (UArray UMatrix))
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((Single
                                                                    ((pattern (Var b))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly)))))
                                                                    (Single
                                                                    ((pattern
                                                                    (Lit Int 1))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly)))))
                                                                    (Single
                                                                    ((pattern
                                                                    (Lit Int 2))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((pattern
                                                                    (FunApp
                                                                    (StanLib sum FnPlain
                                                                    AoS)
                                                                    (((pattern
                                                                    (Indexed
                                                                    ((pattern (Var cov))
                                                                    (meta
                                                                    ((type_
                                                                    (UArray UMatrix))
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((Single
                                                                    ((pattern (Var b))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))))))
                                                                    (meta
                                                                    ((type_ UMatrix)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                   (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                  ((pattern
                                                                    (FunApp
                                                                    (StanLib Times__
                                                                    FnPlain AoS)
                                                                    (((pattern
                                                                    (Promotion
                                                                    ((pattern
                                                                    (Lit Int 5))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))
                                                                    UReal DataOnly))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))
                                                                    ((pattern
                                                                    (Indexed
                                                                    ((pattern (Var cov))
                                                                    (meta
                                                                    ((type_
                                                                    (UArray UMatrix))
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((Single
                                                                    ((pattern (Var b))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly)))))
                                                                    (Single
                                                                    ((pattern
                                                                    (Lit Int 1))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly)))))
                                                                    (Single
                                                                    ((pattern
                                                                    (Lit Int 2))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                   (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                               (meta
                                                                ((type_ UReal)
                                                                 (loc <opaque>)
                                                                 (adlevel AutoDiffable))))
                                                              ((pattern
                                                                (FunApp
                                                                 (StanLib sum FnPlain
                                                                  AoS)
                                                                 (((pattern
                                                                    (Indexed
                                                                    ((pattern
                                                                    (Var chol_corr))
                                                                    (meta
                                                                    ((type_
                                                                    (UArray UMatrix))
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((Single
                                                                    ((pattern (Var b))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))))))
                                                                   (meta
                                                                    ((type_ UMatrix)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                               (meta
                                                                ((type_ UReal)
                                                                 (loc <opaque>)
                                                                 (adlevel AutoDiffable)))))))
                                                           (meta
                                                            ((type_ UReal) 
                                                             (loc <opaque>)
                                                             (adlevel AutoDiffable))))
                                                          ((pattern
                                                            (FunApp
                                                             (StanLib Times__ FnPlain
                                                              AoS)
                                                             (((pattern
                                                                (Promotion
                                                                 ((pattern (Lit Int 7))
                                                                  (meta
                                                                   ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))
                                                                 UReal DataOnly))
                                                               (meta
                                                                ((type_ UReal)
                                                                 (loc <opaque>)
                                                                 (adlevel DataOnly))))
                                                              ((pattern
                                                                (Indexed
                                                                 ((pattern
                                                                   (Var chol_corr))
                                                                  (meta
                                                                   ((type_
                                                                    (UArray UMatrix))
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                 ((Single
                                                                   ((pattern (Var b))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly)))))
                                                                  (Single
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
                                                            ((type_ UReal) 
                                                             (loc <opaque>)
                                                             (adlevel AutoDiffable)))))))
                                                       (meta
                                                        ((type_ UReal) 
                                                         (loc <opaque>)
                                                         (adlevel AutoDiffable))))
                                                      ((pattern
                                                        (FunApp (StanLib sum FnPlain AoS)
                                                         (((pattern
                                                            (Indexed
                                                             ((pattern (Var chol_sq))
                                                              (meta
                                                               ((type_ (UArray UMatrix))
                                                                (loc <opaque>)
                                                                (adlevel AutoDiffable))))
                                                             ((Single
                                                               ((pattern (Var b))
                                                                (meta
                                                                 ((type_ UInt)
                                                                  (loc <opaque>)
                                                                  (adlevel DataOnly))))))))
                                                           (meta
                                                            ((type_ UMatrix)
                                                             (loc <opaque>)
                                                             (adlevel AutoDiffable)))))))
                                                       (meta
                                                        ((type_ UReal) 
                                                         (loc <opaque>)
                                                         (adlevel AutoDiffable)))))))
                                                   (meta
                                                    ((type_ UReal) (loc <opaque>)
                                                     (adlevel AutoDiffable))))
                                                  ((pattern
                                                    (FunApp (StanLib Times__ FnPlain AoS)
                                                     (((pattern
                                                        (Promotion
                                                         ((pattern (Lit Int 11))
                                                          (meta
                                                           ((type_ UInt) 
                                                            (loc <opaque>)
                                                            (adlevel DataOnly))))
                                                         UReal DataOnly))
                                                       (meta
                                                        ((type_ UReal) 
                                                         (loc <opaque>)
                                                         (adlevel DataOnly))))
                                                      ((pattern
                                                        (Indexed
                                                         ((pattern (Var chol_sq))
                                                          (meta
                                                           ((type_ (UArray UMatrix))
                                                            (loc <opaque>)
                                                            (adlevel AutoDiffable))))
                                                         ((Single
                                                           ((pattern (Var b))
                                                            (meta
                                                             ((type_ UInt) 
                                                              (loc <opaque>)
                                                              (adlevel DataOnly)))))
                                                          (Single
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
                                                (FunApp (StanLib sum FnPlain AoS)
                                                 (((pattern
                                                    (Indexed
                                                     ((pattern (Var chol_rect))
                                                      (meta
                                                       ((type_ (UArray UMatrix))
                                                        (loc <opaque>)
                                                        (adlevel AutoDiffable))))
                                                     ((Single
                                                       ((pattern (Var b))
                                                        (meta
                                                         ((type_ UInt) 
                                                          (loc <opaque>)
                                                          (adlevel DataOnly))))))))
                                                   (meta
                                                    ((type_ UMatrix) 
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
                                             (((pattern
                                                (Promotion
                                                 ((pattern (Lit Int 13))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 UReal DataOnly))
                                               (meta
                                                ((type_ UReal) (loc <opaque>)
                                                 (adlevel DataOnly))))
                                              ((pattern
                                                (Indexed
                                                 ((pattern (Var chol_rect))
                                                  (meta
                                                   ((type_ (UArray UMatrix))
                                                    (loc <opaque>)
                                                    (adlevel AutoDiffable))))
                                                 ((Single
                                                   ((pattern (Var b))
                                                    (meta
                                                     ((type_ UInt) (loc <opaque>)
                                                      (adlevel DataOnly)))))
                                                  (Single
                                                   ((pattern (Lit Int 3))
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
                                        ((type_ UReal) (loc <opaque>)
                                         (adlevel AutoDiffable))))
                                      ((pattern
                                        (FunApp (StanLib sum FnPlain AoS)
                                         (((pattern
                                            (Indexed
                                             ((pattern (Var stz_vec))
                                              (meta
                                               ((type_ (UArray UVector)) 
                                                (loc <opaque>) (adlevel AutoDiffable))))
                                             ((Single
                                               ((pattern (Var b))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly))))))))
                                           (meta
                                            ((type_ UVector) (loc <opaque>)
                                             (adlevel AutoDiffable)))))))
                                       (meta
                                        ((type_ UReal) (loc <opaque>)
                                         (adlevel AutoDiffable)))))))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                                  ((pattern
                                    (FunApp (StanLib Times__ FnPlain AoS)
                                     (((pattern
                                        (Promotion
                                         ((pattern (Lit Int 17))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Indexed
                                         ((pattern (Var stz_vec))
                                          (meta
                                           ((type_ (UArray UVector)) 
                                            (loc <opaque>) (adlevel AutoDiffable))))
                                         ((Single
                                           ((pattern (Var b))
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
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                              ((pattern
                                (FunApp (StanLib sum FnPlain AoS)
                                 (((pattern
                                    (Indexed
                                     ((pattern (Var stz_mat))
                                      (meta
                                       ((type_ (UArray UMatrix)) (loc <opaque>)
                                        (adlevel AutoDiffable))))
                                     ((Single
                                       ((pattern (Var b))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                   (meta
                                    ((type_ UMatrix) (loc <opaque>)
                                     (adlevel AutoDiffable)))))))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (FunApp (StanLib Times__ FnPlain AoS)
                             (((pattern
                                (Promotion
                                 ((pattern (Lit Int 19))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Indexed
                                 ((pattern (Var stz_mat))
                                  (meta
                                   ((type_ (UArray UMatrix)) (loc <opaque>)
                                    (adlevel AutoDiffable))))
                                 ((Single
                                   ((pattern (Var b))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                  (Single
                                   ((pattern (Lit Int 1))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                  (Single
                                   ((pattern (Lit Int 1))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern (Var anchor))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (reverse_mode_log_prob
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id simp)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Simplex)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id corr)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Correlation)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id cov)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Covariance)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id chol_corr)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain CholeskyCorr)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id chol_sq)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain CholeskyCov)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id chol_rect)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain CholeskyCov)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id stz_vec)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain SumToZero)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id stz_mat)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain SumToZero)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id stz_row_zero)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain SumToZero)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id stz_col_zero)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain SumToZero)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id anchor) (decl_type (Sized SReal))
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
         (For (loopvar b)
          (lower
           ((pattern (Lit Int 1))
            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (upper
           ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
          (body
           ((pattern
             (Block
              (((pattern
                 (TargetPE
                  ((pattern
                    (FunApp (StanLib Times__ FnPlain AoS)
                     (((pattern
                        (Promotion
                         ((pattern (Var b))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                         UReal DataOnly))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
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
                                            (FunApp (StanLib Plus__ FnPlain AoS)
                                             (((pattern
                                                (FunApp (StanLib Plus__ FnPlain AoS)
                                                 (((pattern
                                                    (FunApp (StanLib Plus__ FnPlain AoS)
                                                     (((pattern
                                                        (FunApp
                                                         (StanLib Plus__ FnPlain AoS)
                                                         (((pattern
                                                            (FunApp
                                                             (StanLib Plus__ FnPlain AoS)
                                                             (((pattern
                                                                (FunApp
                                                                 (StanLib Plus__ FnPlain
                                                                  AoS)
                                                                 (((pattern
                                                                    (FunApp
                                                                    (StanLib Plus__
                                                                    FnPlain AoS)
                                                                    (((pattern
                                                                    (FunApp
                                                                    (StanLib Plus__
                                                                    FnPlain AoS)
                                                                    (((pattern
                                                                    (FunApp
                                                                    (StanLib Plus__
                                                                    FnPlain AoS)
                                                                    (((pattern
                                                                    (FunApp
                                                                    (StanLib Plus__
                                                                    FnPlain AoS)
                                                                    (((pattern
                                                                    (FunApp
                                                                    (StanLib sum FnPlain
                                                                    AoS)
                                                                    (((pattern
                                                                    (Indexed
                                                                    ((pattern (Var simp))
                                                                    (meta
                                                                    ((type_
                                                                    (UArray UVector))
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((Single
                                                                    ((pattern (Var b))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))))))
                                                                    (meta
                                                                    ((type_ UVector)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((pattern
                                                                    (FunApp
                                                                    (StanLib Times__
                                                                    FnPlain AoS)
                                                                    (((pattern
                                                                    (Promotion
                                                                    ((pattern
                                                                    (Lit Int 2))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))
                                                                    UReal DataOnly))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))
                                                                    ((pattern
                                                                    (Indexed
                                                                    ((pattern (Var simp))
                                                                    (meta
                                                                    ((type_
                                                                    (UArray UVector))
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((Single
                                                                    ((pattern (Var b))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly)))))
                                                                    (Single
                                                                    ((pattern
                                                                    (Lit Int 1))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((pattern
                                                                    (FunApp
                                                                    (StanLib sum FnPlain
                                                                    AoS)
                                                                    (((pattern
                                                                    (Indexed
                                                                    ((pattern (Var corr))
                                                                    (meta
                                                                    ((type_
                                                                    (UArray UMatrix))
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((Single
                                                                    ((pattern (Var b))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))))))
                                                                    (meta
                                                                    ((type_ UMatrix)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((pattern
                                                                    (FunApp
                                                                    (StanLib Times__
                                                                    FnPlain AoS)
                                                                    (((pattern
                                                                    (Promotion
                                                                    ((pattern
                                                                    (Lit Int 3))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))
                                                                    UReal DataOnly))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))
                                                                    ((pattern
                                                                    (Indexed
                                                                    ((pattern (Var corr))
                                                                    (meta
                                                                    ((type_
                                                                    (UArray UMatrix))
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((Single
                                                                    ((pattern (Var b))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly)))))
                                                                    (Single
                                                                    ((pattern
                                                                    (Lit Int 1))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly)))))
                                                                    (Single
                                                                    ((pattern
                                                                    (Lit Int 2))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((pattern
                                                                    (FunApp
                                                                    (StanLib sum FnPlain
                                                                    AoS)
                                                                    (((pattern
                                                                    (Indexed
                                                                    ((pattern (Var cov))
                                                                    (meta
                                                                    ((type_
                                                                    (UArray UMatrix))
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((Single
                                                                    ((pattern (Var b))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))))))
                                                                    (meta
                                                                    ((type_ UMatrix)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                   (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                  ((pattern
                                                                    (FunApp
                                                                    (StanLib Times__
                                                                    FnPlain AoS)
                                                                    (((pattern
                                                                    (Promotion
                                                                    ((pattern
                                                                    (Lit Int 5))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))
                                                                    UReal DataOnly))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))
                                                                    ((pattern
                                                                    (Indexed
                                                                    ((pattern (Var cov))
                                                                    (meta
                                                                    ((type_
                                                                    (UArray UMatrix))
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((Single
                                                                    ((pattern (Var b))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly)))))
                                                                    (Single
                                                                    ((pattern
                                                                    (Lit Int 1))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly)))))
                                                                    (Single
                                                                    ((pattern
                                                                    (Lit Int 2))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))))))
                                                                    (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                                   (meta
                                                                    ((type_ UReal)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                               (meta
                                                                ((type_ UReal)
                                                                 (loc <opaque>)
                                                                 (adlevel AutoDiffable))))
                                                              ((pattern
                                                                (FunApp
                                                                 (StanLib sum FnPlain
                                                                  AoS)
                                                                 (((pattern
                                                                    (Indexed
                                                                    ((pattern
                                                                    (Var chol_corr))
                                                                    (meta
                                                                    ((type_
                                                                    (UArray UMatrix))
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                    ((Single
                                                                    ((pattern (Var b))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))))))
                                                                   (meta
                                                                    ((type_ UMatrix)
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable)))))))
                                                               (meta
                                                                ((type_ UReal)
                                                                 (loc <opaque>)
                                                                 (adlevel AutoDiffable)))))))
                                                           (meta
                                                            ((type_ UReal) 
                                                             (loc <opaque>)
                                                             (adlevel AutoDiffable))))
                                                          ((pattern
                                                            (FunApp
                                                             (StanLib Times__ FnPlain
                                                              AoS)
                                                             (((pattern
                                                                (Promotion
                                                                 ((pattern (Lit Int 7))
                                                                  (meta
                                                                   ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly))))
                                                                 UReal DataOnly))
                                                               (meta
                                                                ((type_ UReal)
                                                                 (loc <opaque>)
                                                                 (adlevel DataOnly))))
                                                              ((pattern
                                                                (Indexed
                                                                 ((pattern
                                                                   (Var chol_corr))
                                                                  (meta
                                                                   ((type_
                                                                    (UArray UMatrix))
                                                                    (loc <opaque>)
                                                                    (adlevel
                                                                    AutoDiffable))))
                                                                 ((Single
                                                                   ((pattern (Var b))
                                                                    (meta
                                                                    ((type_ UInt)
                                                                    (loc <opaque>)
                                                                    (adlevel DataOnly)))))
                                                                  (Single
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
                                                            ((type_ UReal) 
                                                             (loc <opaque>)
                                                             (adlevel AutoDiffable)))))))
                                                       (meta
                                                        ((type_ UReal) 
                                                         (loc <opaque>)
                                                         (adlevel AutoDiffable))))
                                                      ((pattern
                                                        (FunApp (StanLib sum FnPlain AoS)
                                                         (((pattern
                                                            (Indexed
                                                             ((pattern (Var chol_sq))
                                                              (meta
                                                               ((type_ (UArray UMatrix))
                                                                (loc <opaque>)
                                                                (adlevel AutoDiffable))))
                                                             ((Single
                                                               ((pattern (Var b))
                                                                (meta
                                                                 ((type_ UInt)
                                                                  (loc <opaque>)
                                                                  (adlevel DataOnly))))))))
                                                           (meta
                                                            ((type_ UMatrix)
                                                             (loc <opaque>)
                                                             (adlevel AutoDiffable)))))))
                                                       (meta
                                                        ((type_ UReal) 
                                                         (loc <opaque>)
                                                         (adlevel AutoDiffable)))))))
                                                   (meta
                                                    ((type_ UReal) (loc <opaque>)
                                                     (adlevel AutoDiffable))))
                                                  ((pattern
                                                    (FunApp (StanLib Times__ FnPlain AoS)
                                                     (((pattern
                                                        (Promotion
                                                         ((pattern (Lit Int 11))
                                                          (meta
                                                           ((type_ UInt) 
                                                            (loc <opaque>)
                                                            (adlevel DataOnly))))
                                                         UReal DataOnly))
                                                       (meta
                                                        ((type_ UReal) 
                                                         (loc <opaque>)
                                                         (adlevel DataOnly))))
                                                      ((pattern
                                                        (Indexed
                                                         ((pattern (Var chol_sq))
                                                          (meta
                                                           ((type_ (UArray UMatrix))
                                                            (loc <opaque>)
                                                            (adlevel AutoDiffable))))
                                                         ((Single
                                                           ((pattern (Var b))
                                                            (meta
                                                             ((type_ UInt) 
                                                              (loc <opaque>)
                                                              (adlevel DataOnly)))))
                                                          (Single
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
                                                (FunApp (StanLib sum FnPlain AoS)
                                                 (((pattern
                                                    (Indexed
                                                     ((pattern (Var chol_rect))
                                                      (meta
                                                       ((type_ (UArray UMatrix))
                                                        (loc <opaque>)
                                                        (adlevel AutoDiffable))))
                                                     ((Single
                                                       ((pattern (Var b))
                                                        (meta
                                                         ((type_ UInt) 
                                                          (loc <opaque>)
                                                          (adlevel DataOnly))))))))
                                                   (meta
                                                    ((type_ UMatrix) 
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
                                             (((pattern
                                                (Promotion
                                                 ((pattern (Lit Int 13))
                                                  (meta
                                                   ((type_ UInt) (loc <opaque>)
                                                    (adlevel DataOnly))))
                                                 UReal DataOnly))
                                               (meta
                                                ((type_ UReal) (loc <opaque>)
                                                 (adlevel DataOnly))))
                                              ((pattern
                                                (Indexed
                                                 ((pattern (Var chol_rect))
                                                  (meta
                                                   ((type_ (UArray UMatrix))
                                                    (loc <opaque>)
                                                    (adlevel AutoDiffable))))
                                                 ((Single
                                                   ((pattern (Var b))
                                                    (meta
                                                     ((type_ UInt) (loc <opaque>)
                                                      (adlevel DataOnly)))))
                                                  (Single
                                                   ((pattern (Lit Int 3))
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
                                        ((type_ UReal) (loc <opaque>)
                                         (adlevel AutoDiffable))))
                                      ((pattern
                                        (FunApp (StanLib sum FnPlain AoS)
                                         (((pattern
                                            (Indexed
                                             ((pattern (Var stz_vec))
                                              (meta
                                               ((type_ (UArray UVector)) 
                                                (loc <opaque>) (adlevel AutoDiffable))))
                                             ((Single
                                               ((pattern (Var b))
                                                (meta
                                                 ((type_ UInt) (loc <opaque>)
                                                  (adlevel DataOnly))))))))
                                           (meta
                                            ((type_ UVector) (loc <opaque>)
                                             (adlevel AutoDiffable)))))))
                                       (meta
                                        ((type_ UReal) (loc <opaque>)
                                         (adlevel AutoDiffable)))))))
                                   (meta
                                    ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                                  ((pattern
                                    (FunApp (StanLib Times__ FnPlain AoS)
                                     (((pattern
                                        (Promotion
                                         ((pattern (Lit Int 17))
                                          (meta
                                           ((type_ UInt) (loc <opaque>)
                                            (adlevel DataOnly))))
                                         UReal DataOnly))
                                       (meta
                                        ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                                      ((pattern
                                        (Indexed
                                         ((pattern (Var stz_vec))
                                          (meta
                                           ((type_ (UArray UVector)) 
                                            (loc <opaque>) (adlevel AutoDiffable))))
                                         ((Single
                                           ((pattern (Var b))
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
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                              ((pattern
                                (FunApp (StanLib sum FnPlain AoS)
                                 (((pattern
                                    (Indexed
                                     ((pattern (Var stz_mat))
                                      (meta
                                       ((type_ (UArray UMatrix)) (loc <opaque>)
                                        (adlevel AutoDiffable))))
                                     ((Single
                                       ((pattern (Var b))
                                        (meta
                                         ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                   (meta
                                    ((type_ UMatrix) (loc <opaque>)
                                     (adlevel AutoDiffable)))))))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))
                          ((pattern
                            (FunApp (StanLib Times__ FnPlain AoS)
                             (((pattern
                                (Promotion
                                 ((pattern (Lit Int 19))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                                 UReal DataOnly))
                               (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))
                              ((pattern
                                (Indexed
                                 ((pattern (Var stz_mat))
                                  (meta
                                   ((type_ (UArray UMatrix)) (loc <opaque>)
                                    (adlevel AutoDiffable))))
                                 ((Single
                                   ((pattern (Var b))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                  (Single
                                   ((pattern (Lit Int 1))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                  (Single
                                   ((pattern (Lit Int 1))
                                    (meta
                                     ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                               (meta
                                ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable)))))))
                   (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>))
       ((pattern
         (TargetPE
          ((pattern (Var anchor))
           (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
        (meta <opaque>)))))
    (meta <opaque>))))
 (generate_quantities
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id simp)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Simplex)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id corr)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Correlation)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id cov)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain Covariance)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id chol_corr)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain CholeskyCorr)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id chol_sq)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain CholeskyCov)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id chol_rect)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain CholeskyCov)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id stz_vec)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain SumToZero)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id stz_mat)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain SumToZero)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 2))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id stz_row_zero)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain SumToZero)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id stz_col_zero)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize
       (Assign
        ((pattern
          (FunApp
           (CompilerInternal
            (FnReadParam (constrain SumToZero)
             (dims
              (((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 3))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
             (mem_pattern AoS)))
           ()))
         (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel AutoDiffable))))))))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype DataOnly) (decl_id anchor) (decl_type (Sized SReal))
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
     (For (loopvar sym1__)
      (lower
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (upper
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (body
       ((pattern
         (Block
          (((pattern
             (For (loopvar sym2__)
              (lower
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (upper
               ((pattern (Var B))
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
                            ((pattern (Var simp))
                             (meta
                              ((type_ (UArray UVector)) (loc <opaque>)
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
                       ((pattern (Var B))
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
                                    ((pattern (Var corr))
                                     (meta
                                      ((type_ (UArray UMatrix)) (loc <opaque>)
                                       (adlevel DataOnly))))
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
                                        ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                  (meta
                                   ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                              ()))
                            (meta <opaque>)))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
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
                       ((pattern (Var B))
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
                                    ((pattern (Var cov))
                                     (meta
                                      ((type_ (UArray UMatrix)) (loc <opaque>)
                                       (adlevel DataOnly))))
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
                                        ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                  (meta
                                   ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                              ()))
                            (meta <opaque>)))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
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
                       ((pattern (Var B))
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
                                    ((pattern (Var chol_corr))
                                     (meta
                                      ((type_ (UArray UMatrix)) (loc <opaque>)
                                       (adlevel DataOnly))))
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
                                        ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                  (meta
                                   ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                              ()))
                            (meta <opaque>)))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
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
                       ((pattern (Var B))
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
                                    ((pattern (Var chol_sq))
                                     (meta
                                      ((type_ (UArray UMatrix)) (loc <opaque>)
                                       (adlevel DataOnly))))
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
                                        ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                  (meta
                                   ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                              ()))
                            (meta <opaque>)))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
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
               ((pattern (Lit Int 3))
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
                       ((pattern (Var B))
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
                                    ((pattern (Var chol_rect))
                                     (meta
                                      ((type_ (UArray UMatrix)) (loc <opaque>)
                                       (adlevel DataOnly))))
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
                                        ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                  (meta
                                   ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                              ()))
                            (meta <opaque>)))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))
   ((pattern
     (For (loopvar sym1__)
      (lower
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (upper
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (body
       ((pattern
         (Block
          (((pattern
             (For (loopvar sym2__)
              (lower
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (upper
               ((pattern (Var B))
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
                            ((pattern (Var stz_vec))
                             (meta
                              ((type_ (UArray UVector)) (loc <opaque>)
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
    (meta <opaque>))
   ((pattern
     (For (loopvar sym1__)
      (lower
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (upper
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
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
                       ((pattern (Var B))
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
                                    ((pattern (Var stz_mat))
                                     (meta
                                      ((type_ (UArray UMatrix)) (loc <opaque>)
                                       (adlevel DataOnly))))
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
                                        ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                  (meta
                                   ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                              ()))
                            (meta <opaque>)))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))
   ((pattern
     (For (loopvar sym1__)
      (lower
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (upper
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (body
       ((pattern
         (Block
          (((pattern
             (For (loopvar sym2__)
              (lower
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (upper
               ((pattern (Lit Int 1))
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
                       ((pattern (Var B))
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
                                    ((pattern (Var stz_row_zero))
                                     (meta
                                      ((type_ (UArray UMatrix)) (loc <opaque>)
                                       (adlevel DataOnly))))
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
                                        ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                  (meta
                                   ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                              ()))
                            (meta <opaque>)))))
                        (meta <opaque>)))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))
   ((pattern
     (For (loopvar sym1__)
      (lower
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (upper
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
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
                     (For (loopvar sym3__)
                      (lower
                       ((pattern (Lit Int 1))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                      (upper
                       ((pattern (Var B))
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
                                    ((pattern (Var stz_col_zero))
                                     (meta
                                      ((type_ (UArray UMatrix)) (loc <opaque>)
                                       (adlevel DataOnly))))
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
                                        ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))))
                                  (meta
                                   ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
                              ()))
                            (meta <opaque>)))))
                        (meta <opaque>)))))
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
         ((pattern (Var anchor))
          (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
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
     (Decl (decl_adtype DataOnly) (decl_id proof) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable proof) ()) UReal
      ((pattern (Var anchor)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt ())
        (var
         ((pattern (Var proof)) (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
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
     (Decl (decl_adtype AutoDiffable) (decl_id simp)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id simp_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable simp_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str simp))
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
                   ((pattern (Var B))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
                         (Assignment
                          ((LVariable simp)
                           ((Single
                             ((pattern (Var sym2__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                            (Single
                             ((pattern (Var sym1__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (UArray UVector)
                          ((pattern
                            (Indexed
                             ((pattern (Var simp_flat__))
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
       (FnWriteParam (unconstrain_opt (Simplex))
        (var
         ((pattern (Var simp))
          (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id corr)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id corr_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable corr_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str corr))
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
                           ((pattern (Var B))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (body
                           ((pattern
                             (Block
                              (((pattern
                                 (Assignment
                                  ((LVariable corr)
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
                                     ((pattern (Var corr_flat__))
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Correlation))
        (var
         ((pattern (Var corr))
          (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id cov)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id cov_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable cov_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str cov))
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
                           ((pattern (Var B))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (body
                           ((pattern
                             (Block
                              (((pattern
                                 (Assignment
                                  ((LVariable cov)
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
                                     ((pattern (Var cov_flat__))
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Covariance))
        (var
         ((pattern (Var cov))
          (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id chol_corr)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id chol_corr_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable chol_corr_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str chol_corr))
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
                           ((pattern (Var B))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (body
                           ((pattern
                             (Block
                              (((pattern
                                 (Assignment
                                  ((LVariable chol_corr)
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
                                     ((pattern (Var chol_corr_flat__))
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (CholeskyCorr))
        (var
         ((pattern (Var chol_corr))
          (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id chol_sq)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id chol_sq_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable chol_sq_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str chol_sq))
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
                           ((pattern (Var B))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (body
                           ((pattern
                             (Block
                              (((pattern
                                 (Assignment
                                  ((LVariable chol_sq)
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
                                     ((pattern (Var chol_sq_flat__))
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (CholeskyCov))
        (var
         ((pattern (Var chol_sq))
          (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id chol_rect)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id chol_rect_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable chol_rect_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str chol_rect))
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
                         (For (loopvar sym3__)
                          (lower
                           ((pattern (Lit Int 1))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (upper
                           ((pattern (Var B))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (body
                           ((pattern
                             (Block
                              (((pattern
                                 (Assignment
                                  ((LVariable chol_rect)
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
                                     ((pattern (Var chol_rect_flat__))
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (CholeskyCov))
        (var
         ((pattern (Var chol_rect))
          (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id stz_vec)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id stz_vec_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable stz_vec_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str stz_vec))
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
                   ((pattern (Var B))
                    (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                  (body
                   ((pattern
                     (Block
                      (((pattern
                         (Assignment
                          ((LVariable stz_vec)
                           ((Single
                             ((pattern (Var sym2__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                            (Single
                             ((pattern (Var sym1__))
                              (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                          (UArray UVector)
                          ((pattern
                            (Indexed
                             ((pattern (Var stz_vec_flat__))
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
       (FnWriteParam (unconstrain_opt (SumToZero))
        (var
         ((pattern (Var stz_vec))
          (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id stz_mat)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id stz_mat_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable stz_mat_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str stz_mat))
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
                           ((pattern (Var B))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (body
                           ((pattern
                             (Block
                              (((pattern
                                 (Assignment
                                  ((LVariable stz_mat)
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
                                     ((pattern (Var stz_mat_flat__))
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (SumToZero))
        (var
         ((pattern (Var stz_mat))
          (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id stz_row_zero)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id stz_row_zero_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable stz_row_zero_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str stz_row_zero))
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
                   ((pattern (Lit Int 1))
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
                           ((pattern (Var B))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (body
                           ((pattern
                             (Block
                              (((pattern
                                 (Assignment
                                  ((LVariable stz_row_zero)
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
                                     ((pattern (Var stz_row_zero_flat__))
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (SumToZero))
        (var
         ((pattern (Var stz_row_zero))
          (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id stz_col_zero)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Block
      (((pattern
         (Decl (decl_adtype AutoDiffable) (decl_id stz_col_zero_flat__)
          (decl_type (Unsized (UArray UReal))) (initialize Uninit)))
        (meta <opaque>))
       ((pattern
         (Assignment ((LVariable stz_col_zero_flat__) ()) (UArray UReal)
          ((pattern
            (FunApp (CompilerInternal FnReadData)
             (((pattern (Lit Str stz_col_zero))
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
           ((pattern (Lit Int 1))
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
                         (For (loopvar sym3__)
                          (lower
                           ((pattern (Lit Int 1))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (upper
                           ((pattern (Var B))
                            (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                          (body
                           ((pattern
                             (Block
                              (((pattern
                                 (Assignment
                                  ((LVariable stz_col_zero)
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
                                     ((pattern (Var stz_col_zero_flat__))
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (SumToZero))
        (var
         ((pattern (Var stz_col_zero))
          (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id anchor) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable anchor) ()) UReal
      ((pattern
        (Indexed
         ((pattern
           (FunApp (CompilerInternal FnReadData)
            (((pattern (Lit Str anchor))
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
         ((pattern (Var anchor))
          (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (unconstrain_array
  (((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id simp)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (For (loopvar sym1__)
      (lower
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (upper
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (body
       ((pattern
         (Block
          (((pattern
             (For (loopvar sym2__)
              (lower
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (upper
               ((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (body
               ((pattern
                 (Block
                  (((pattern
                     (Assignment
                      ((LVariable simp)
                       ((Single
                         ((pattern (Var sym2__))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                        (Single
                         ((pattern (Var sym1__))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                      UVector
                      ((pattern (FunApp (CompilerInternal FnReadDeserializer) ()))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Simplex))
        (var
         ((pattern (Var simp))
          (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id corr)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
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
                       ((pattern (Var B))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                      (body
                       ((pattern
                         (Block
                          (((pattern
                             (Assignment
                              ((LVariable corr)
                               ((Single
                                 ((pattern (Var sym3__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var sym2__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var sym1__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              UMatrix
                              ((pattern
                                (FunApp (CompilerInternal FnReadDeserializer) ()))
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Correlation))
        (var
         ((pattern (Var corr))
          (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id cov)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
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
                       ((pattern (Var B))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                      (body
                       ((pattern
                         (Block
                          (((pattern
                             (Assignment
                              ((LVariable cov)
                               ((Single
                                 ((pattern (Var sym3__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var sym2__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var sym1__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              UMatrix
                              ((pattern
                                (FunApp (CompilerInternal FnReadDeserializer) ()))
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Covariance))
        (var
         ((pattern (Var cov))
          (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id chol_corr)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
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
                       ((pattern (Var B))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                      (body
                       ((pattern
                         (Block
                          (((pattern
                             (Assignment
                              ((LVariable chol_corr)
                               ((Single
                                 ((pattern (Var sym3__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var sym2__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var sym1__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              UMatrix
                              ((pattern
                                (FunApp (CompilerInternal FnReadDeserializer) ()))
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (CholeskyCorr))
        (var
         ((pattern (Var chol_corr))
          (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id chol_sq)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
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
                       ((pattern (Var B))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                      (body
                       ((pattern
                         (Block
                          (((pattern
                             (Assignment
                              ((LVariable chol_sq)
                               ((Single
                                 ((pattern (Var sym3__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var sym2__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var sym1__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              UMatrix
                              ((pattern
                                (FunApp (CompilerInternal FnReadDeserializer) ()))
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (CholeskyCov))
        (var
         ((pattern (Var chol_sq))
          (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id chol_rect)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
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
               ((pattern (Lit Int 3))
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
                       ((pattern (Var B))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                      (body
                       ((pattern
                         (Block
                          (((pattern
                             (Assignment
                              ((LVariable chol_rect)
                               ((Single
                                 ((pattern (Var sym3__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var sym2__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var sym1__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              UMatrix
                              ((pattern
                                (FunApp (CompilerInternal FnReadDeserializer) ()))
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (CholeskyCov))
        (var
         ((pattern (Var chol_rect))
          (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id stz_vec)
      (decl_type
       (Sized
        (SArray
         (SVector AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (For (loopvar sym1__)
      (lower
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (upper
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (body
       ((pattern
         (Block
          (((pattern
             (For (loopvar sym2__)
              (lower
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (upper
               ((pattern (Var B))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (body
               ((pattern
                 (Block
                  (((pattern
                     (Assignment
                      ((LVariable stz_vec)
                       ((Single
                         ((pattern (Var sym2__))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                        (Single
                         ((pattern (Var sym1__))
                          (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                      UVector
                      ((pattern (FunApp (CompilerInternal FnReadDeserializer) ()))
                       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
                    (meta <opaque>)))))
                (meta <opaque>)))))
            (meta <opaque>)))))
        (meta <opaque>)))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (SumToZero))
        (var
         ((pattern (Var stz_vec))
          (meta ((type_ (UArray UVector)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id stz_mat)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (For (loopvar sym1__)
      (lower
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (upper
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
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
                       ((pattern (Var B))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                      (body
                       ((pattern
                         (Block
                          (((pattern
                             (Assignment
                              ((LVariable stz_mat)
                               ((Single
                                 ((pattern (Var sym3__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var sym2__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var sym1__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              UMatrix
                              ((pattern
                                (FunApp (CompilerInternal FnReadDeserializer) ()))
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (SumToZero))
        (var
         ((pattern (Var stz_mat))
          (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id stz_row_zero)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (For (loopvar sym1__)
      (lower
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (upper
       ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (body
       ((pattern
         (Block
          (((pattern
             (For (loopvar sym2__)
              (lower
               ((pattern (Lit Int 1))
                (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
              (upper
               ((pattern (Lit Int 1))
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
                       ((pattern (Var B))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                      (body
                       ((pattern
                         (Block
                          (((pattern
                             (Assignment
                              ((LVariable stz_row_zero)
                               ((Single
                                 ((pattern (Var sym3__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var sym2__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var sym1__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              UMatrix
                              ((pattern
                                (FunApp (CompilerInternal FnReadDeserializer) ()))
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (SumToZero))
        (var
         ((pattern (Var stz_row_zero))
          (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id stz_col_zero)
      (decl_type
       (Sized
        (SArray
         (SMatrix AoS
          ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
          ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
         ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (For (loopvar sym1__)
      (lower
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
      (upper
       ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
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
                     (For (loopvar sym3__)
                      (lower
                       ((pattern (Lit Int 1))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                      (upper
                       ((pattern (Var B))
                        (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                      (body
                       ((pattern
                         (Block
                          (((pattern
                             (Assignment
                              ((LVariable stz_col_zero)
                               ((Single
                                 ((pattern (Var sym3__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var sym2__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
                                (Single
                                 ((pattern (Var sym1__))
                                  (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                              UMatrix
                              ((pattern
                                (FunApp (CompilerInternal FnReadDeserializer) ()))
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
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (SumToZero))
        (var
         ((pattern (Var stz_col_zero))
          (meta ((type_ (UArray UMatrix)) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))
   ((pattern
     (Decl (decl_adtype AutoDiffable) (decl_id anchor) (decl_type (Sized SReal))
      (initialize Default)))
    (meta <opaque>))
   ((pattern
     (Assignment ((LVariable anchor) ()) UReal
      ((pattern (FunApp (CompilerInternal FnReadDeserializer) ()))
       (meta ((type_ UReal) (loc <opaque>) (adlevel AutoDiffable))))))
    (meta <opaque>))
   ((pattern
     (NRFunApp
      (CompilerInternal
       (FnWriteParam (unconstrain_opt (Identity))
        (var
         ((pattern (Var anchor))
          (meta ((type_ UReal) (loc <opaque>) (adlevel DataOnly)))))))
      ()))
    (meta <opaque>))))
 (output_vars
  ((simp <opaque>
    ((out_unconstrained_st
      (SArray
       (SVector AoS
        ((pattern
          (FunApp (StanLib Minus__ FnPlain AoS)
           (((pattern (Lit Int 3))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
            ((pattern (Lit Int 1))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SVector AoS
        ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans Simplex)))
   (corr <opaque>
    ((out_unconstrained_st
      (SArray
       (SVector AoS
        ((pattern
          (FunApp (StanLib Divide__ FnPlain AoS)
           (((pattern
              (FunApp (StanLib Times__ FnPlain AoS)
               (((pattern (Lit Int 2))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                ((pattern
                  (FunApp (StanLib Minus__ FnPlain AoS)
                   (((pattern (Lit Int 2))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                    ((pattern (Lit Int 1))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
            ((pattern (Lit Int 2))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SMatrix AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans Correlation)))
   (cov <opaque>
    ((out_unconstrained_st
      (SArray
       (SVector AoS
        ((pattern
          (FunApp (StanLib Plus__ FnPlain AoS)
           (((pattern (Lit Int 2))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
            ((pattern
              (FunApp (StanLib Divide__ FnPlain AoS)
               (((pattern
                  (FunApp (StanLib Times__ FnPlain AoS)
                   (((pattern (Lit Int 2))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                    ((pattern
                      (FunApp (StanLib Minus__ FnPlain AoS)
                       (((pattern (Lit Int 2))
                         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                        ((pattern (Lit Int 1))
                         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                ((pattern (Lit Int 2))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SMatrix AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans Covariance)))
   (chol_corr <opaque>
    ((out_unconstrained_st
      (SArray
       (SVector AoS
        ((pattern
          (FunApp (StanLib Divide__ FnPlain AoS)
           (((pattern
              (FunApp (StanLib Times__ FnPlain AoS)
               (((pattern (Lit Int 2))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                ((pattern
                  (FunApp (StanLib Minus__ FnPlain AoS)
                   (((pattern (Lit Int 2))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                    ((pattern (Lit Int 1))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
            ((pattern (Lit Int 2))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SMatrix AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans CholeskyCorr)))
   (chol_sq <opaque>
    ((out_unconstrained_st
      (SArray
       (SVector AoS
        ((pattern
          (FunApp (StanLib Plus__ FnPlain AoS)
           (((pattern
              (FunApp (StanLib Plus__ FnPlain AoS)
               (((pattern
                  (FunApp (StanLib Divide__ FnPlain AoS)
                   (((pattern
                      (FunApp (StanLib Times__ FnPlain AoS)
                       (((pattern (Lit Int 2))
                         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                        ((pattern
                          (FunApp (StanLib Minus__ FnPlain AoS)
                           (((pattern (Lit Int 2))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                            ((pattern (Lit Int 1))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                    ((pattern (Lit Int 2))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                ((pattern (Lit Int 2))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
            ((pattern
              (FunApp (StanLib Times__ FnPlain AoS)
               (((pattern
                  (FunApp (StanLib Minus__ FnPlain AoS)
                   (((pattern (Lit Int 2))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                    ((pattern (Lit Int 2))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                ((pattern (Lit Int 2))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SMatrix AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans CholeskyCov)))
   (chol_rect <opaque>
    ((out_unconstrained_st
      (SArray
       (SVector AoS
        ((pattern
          (FunApp (StanLib Plus__ FnPlain AoS)
           (((pattern
              (FunApp (StanLib Plus__ FnPlain AoS)
               (((pattern
                  (FunApp (StanLib Divide__ FnPlain AoS)
                   (((pattern
                      (FunApp (StanLib Times__ FnPlain AoS)
                       (((pattern (Lit Int 2))
                         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                        ((pattern
                          (FunApp (StanLib Minus__ FnPlain AoS)
                           (((pattern (Lit Int 2))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                            ((pattern (Lit Int 1))
                             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                    ((pattern (Lit Int 2))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                ((pattern (Lit Int 2))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
            ((pattern
              (FunApp (StanLib Times__ FnPlain AoS)
               (((pattern
                  (FunApp (StanLib Minus__ FnPlain AoS)
                   (((pattern (Lit Int 3))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                    ((pattern (Lit Int 2))
                     (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
                ((pattern (Lit Int 2))
                 (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SMatrix AoS
        ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans CholeskyCov)))
   (stz_vec <opaque>
    ((out_unconstrained_st
      (SArray
       (SVector AoS
        ((pattern
          (FunApp (StanLib Minus__ FnPlain AoS)
           (((pattern (Lit Int 3))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
            ((pattern (Lit Int 1))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SVector AoS
        ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans SumToZero)))
   (stz_mat <opaque>
    ((out_unconstrained_st
      (SArray
       (SMatrix AoS
        ((pattern
          (FunApp (StanLib Minus__ FnPlain AoS)
           (((pattern (Lit Int 2))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
            ((pattern (Lit Int 1))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern
          (FunApp (StanLib Minus__ FnPlain AoS)
           (((pattern (Lit Int 3))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
            ((pattern (Lit Int 1))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SMatrix AoS
        ((pattern (Lit Int 2)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans SumToZero)))
   (stz_row_zero <opaque>
    ((out_unconstrained_st
      (SArray
       (SMatrix AoS
        ((pattern
          (FunApp (StanLib Minus__ FnPlain AoS)
           (((pattern (Lit Int 1))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
            ((pattern (Lit Int 1))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern
          (FunApp (StanLib Minus__ FnPlain AoS)
           (((pattern (Lit Int 3))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
            ((pattern (Lit Int 1))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SMatrix AoS
        ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans SumToZero)))
   (stz_col_zero <opaque>
    ((out_unconstrained_st
      (SArray
       (SMatrix AoS
        ((pattern
          (FunApp (StanLib Minus__ FnPlain AoS)
           (((pattern (Lit Int 3))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
            ((pattern (Lit Int 1))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern
          (FunApp (StanLib Minus__ FnPlain AoS)
           (((pattern (Lit Int 1))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
            ((pattern (Lit Int 1))
             (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))))
         (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_constrained_st
      (SArray
       (SMatrix AoS
        ((pattern (Lit Int 3)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))
        ((pattern (Lit Int 1)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly)))))
       ((pattern (Var B)) (meta ((type_ UInt) (loc <opaque>) (adlevel DataOnly))))))
     (out_block Parameters) (out_trans SumToZero)))
   (anchor <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal) (out_block Parameters)
     (out_trans Identity)))
   (proof <opaque>
    ((out_unconstrained_st SReal) (out_constrained_st SReal)
     (out_block GeneratedQuantities) (out_trans Identity)))))
 (prog_name structured_arrays_model) (prog_path tests/fixtures/structured_arrays.stan))