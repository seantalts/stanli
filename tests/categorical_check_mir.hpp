#ifndef STANLI_TEST_CATEGORICAL_CHECK_MIR_HPP
#define STANLI_TEST_CATEGORICAL_CHECK_MIR_HPP

#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>

// A minimal hand-written transformed-MIR program for one categorical call.
// Keeping this as text makes the behavior tests independent of stanc's
// generated fixture output while still entering through the public MIR reader
// and lowering path.
inline std::string categorical_check_mir(const std::string& function,
                                         bool scalar_outcome, int outcome_size,
                                         int arg_size, bool propto = true,
                                         bool autodiff_local_arg = false,
                                         bool nested_outcome_binding = false,
                                         bool wrap_density_udf = false) {
  std::ostringstream out;
  out << "((functions_block ())\n"
      << " (input_vars\n  ((outcome <opaque> ";
  if (scalar_outcome) {
    out << "SInt";
  } else if (nested_outcome_binding) {
    out << "(SArray\n"
        << "     (SArray SInt ((pattern (Lit Int " << outcome_size
        << ")) (meta ((type_ UInt) (adlevel DataOnly)))))\n"
        << "     ((pattern (Lit Int 1)) (meta ((type_ UInt) (adlevel "
           "DataOnly)))))";
  } else {
    out << "(SArray SInt ((pattern (Lit Int " << outcome_size
        << ")) (meta ((type_ UInt) (adlevel DataOnly)))))";
  }
  out << ")\n   (arg <opaque>\n    (SVector AoS\n     ((pattern (Lit Int "
      << arg_size << ")) (meta ((type_ UInt) (adlevel DataOnly))))))))\n"
      << " (prepare_data ())\n"
      << " (log_prob\n"
      << "  (((pattern\n"
      << "     (Decl (decl_adtype AutoDiffable) (decl_id anchor)\n"
      << "      (decl_type (Sized SReal))\n"
      << "      (initialize\n"
      << "       (Assign\n"
      << "        ((pattern\n"
      << "          (FunApp\n"
      << "           (CompilerInternal\n"
      << "            (FnReadParam (constrain Identity) (dims ())\n"
      << "             (mem_pattern AoS)))\n"
      << "           ()))\n"
      << "         (meta ((type_ UReal) (adlevel AutoDiffable))))))))\n"
      << "    (meta <opaque>))\n"
      << "   ((pattern\n"
      << "     (TargetPE\n"
      << "      ((pattern (Var anchor))\n"
      << "       (meta ((type_ UReal) (adlevel AutoDiffable))))))\n"
      << "    (meta <opaque>))\n";
  if (autodiff_local_arg) {
    out << "   ((pattern\n"
        << "     (Decl (decl_adtype AutoDiffable) (decl_id local_arg)\n"
        << "      (decl_type\n"
        << "       (Sized\n"
        << "        (SVector AoS\n"
        << "         ((pattern (Lit Int " << arg_size
        << ")) (meta ((type_ UInt) (adlevel DataOnly)))))))\n"
        << "      (initialize Default)))\n"
        << "    (meta <opaque>))\n"
        << "   ((pattern\n"
        << "     (Assignment ((LVariable local_arg) ()) UVector\n"
        << "      ((pattern (Var arg))\n"
        << "       (meta ((type_ UVector) (adlevel AutoDiffable))))))\n"
        << "    (meta <opaque>))\n";
  }
  out << "   ((pattern\n     (TargetPE\n      ((pattern\n";
  if (wrap_density_udf)
    out << "        (FunApp (UserDefined identity_real FnPlain)\n"
        << "         (((pattern\n";
  out << (wrap_density_udf ? "            " : "        ") << "(FunApp (StanLib "
      << function << " (FnLpmf " << (propto ? "true" : "false") << ") AoS)\n"
      << (wrap_density_udf ? "             " : "         ")
      << "(((pattern (Var outcome))\n"
      << (wrap_density_udf ? "               " : "           ")
      << "(meta ((type_ " << (scalar_outcome ? "UInt" : "(UArray UInt)")
      << ") (adlevel DataOnly))))\n"
      << (wrap_density_udf ? "              " : "          ")
      << "((pattern (Var " << (autodiff_local_arg ? "local_arg" : "arg")
      << "))\n"
      << (wrap_density_udf ? "               " : "           ")
      << "(meta ((type_ UVector) (adlevel "
      << (autodiff_local_arg ? "AutoDiffable" : "DataOnly") << ")))))))\n";
  if (wrap_density_udf)
    out << "           (meta ((type_ UReal) (adlevel DataOnly)))))))\n";
  out << "       (meta ((type_ UReal) (adlevel "
      << (autodiff_local_arg ? "AutoDiffable" : "DataOnly") << "))))))\n"
      << "    (meta <opaque>)))))\n";
  return out.str();
}

inline std::string categorical_effect_check_mir() {
  std::string mir = categorical_check_mir("categorical_lpmf", true, 1, 3);
  const std::string empty_functions = "(functions_block ())";
  const std::string functions = R"((functions_block
  (((fdrt (ReturnType UInt)) (fdname noisy) (fdsuffix FnPlain)
    (fdargs ((DataOnly x UInt)))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (NRFunApp (CompilerInternal FnPrint)
             (((pattern (Lit Str "categorical effect"))
               (meta ((type_ UReal) (adlevel DataOnly)))))))
           (meta <opaque>))
          ((pattern
            (Return
             (((pattern (Var x))
               (meta ((type_ UInt) (adlevel DataOnly)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>)))))";
  mir.replace(mir.find(empty_functions), empty_functions.size(), functions);

  const std::string outcome =
      "((pattern (Var outcome))\n           (meta ((type_ UInt) (adlevel "
      "DataOnly))))";
  const std::string call =
      "((pattern\n            (FunApp (UserDefined noisy FnPlain)\n"
      "             (((pattern (Var outcome))\n"
      "               (meta ((type_ UInt) (adlevel DataOnly)))))))\n"
      "           (meta ((type_ UInt) (adlevel DataOnly))))";
  mir.replace(mir.find(outcome), outcome.size(), call);
  return mir;
}

inline std::string categorical_udf_actual_mir(const std::string& function) {
  std::string mir =
      categorical_check_mir(function, true, 1, 3, false, false, false, true);
  const std::string empty_functions = "(functions_block ())";
  const std::string functions = R"((functions_block
  (((fdrt (ReturnType UReal)) (fdname identity_real) (fdsuffix FnPlain)
    (fdargs ((DataOnly x UReal)))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Return
             (((pattern (Var x))
               (meta ((type_ UReal) (adlevel DataOnly)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>)))))";
  mir.replace(mir.find(empty_functions), empty_functions.size(), functions);

  return mir;
}

inline std::string categorical_udf_mir(const std::string& function, bool propto,
                                       bool autodiff_local_arg,
                                       bool udf_body_local = false,
                                       bool data_formal = false) {
  std::string mir =
      categorical_check_mir(function, true, 1, 3, propto, autodiff_local_arg);
  const std::string udf_name =
      function == "categorical_lpmf" ? "cat_lpmf" : "cat_logit_lpmf";
  std::ostringstream functions;
  functions << "(functions_block\n"
            << "  (((fdrt (ReturnType UReal)) (fdname " << udf_name
            << ") (fdsuffix (FnLpmf ()))\n"
            << "    (fdargs ((DataOnly y UInt) ("
            << (data_formal ? "DataOnly" : "AutoDiffable") << " p UVector)))\n"
            << "    (fdbody\n"
            << "     (((pattern\n"
            << "        (Block\n"
            << "         (((pattern\n"
            << "            (Return\n"
            << "             (((pattern\n"
            << "                (FunApp (StanLib " << function
            << " (FnLpmf true) AoS)\n"
            << "                 (((pattern (Var y))\n"
            << "                   (meta ((type_ UInt) (adlevel DataOnly))))\n"
            << "                  ((pattern (Var p))\n"
            << "                   (meta ((type_ UVector) (adlevel "
               "AutoDiffable)))))))\n"
            << "               (meta ((type_ UReal) (adlevel "
               "AutoDiffable)))))))\n"
            << "           (meta <opaque>)))))\n"
            << "       (meta <opaque>))))\n"
            << "    (fdloc <opaque>))))";
  const std::string empty_functions = "(functions_block ())";
  mir.replace(mir.find(empty_functions), empty_functions.size(),
              functions.str());

  if (udf_body_local) {
    const std::string return_marker =
        "         (((pattern\n"
        "            (Return";
    const size_t ret = mir.find(return_marker);
    if (ret == std::string::npos)
      throw std::logic_error("categorical UDF fixture has no return");
    const std::string local =
        "         (((pattern\n"
        "            (Decl (decl_adtype AutoDiffable) (decl_id q)\n"
        "             (decl_type\n"
        "              (Sized\n"
        "               (SVector AoS\n"
        "                ((pattern (Lit Int 3))\n"
        "                 (meta ((type_ UInt) (adlevel DataOnly)))))))\n"
        "             (initialize\n"
        "              (Assign\n"
        "               ((pattern (Var p))\n"
        "                (meta ((type_ UVector) (adlevel "
        "AutoDiffable))))))))\n"
        "           (meta <opaque>))\n"
        "          ((pattern\n"
        "            (Return";
    mir.replace(ret, return_marker.size(), local);
    const size_t local_return = mir.find("(Return", ret);
    const size_t p = mir.find("(Var p)", local_return);
    if (p == std::string::npos)
      throw std::logic_error("categorical UDF fixture has no return argument");
    mir.replace(p, std::strlen("(Var p)"), "(Var q)");
  }

  const size_t log_prob = mir.find("(log_prob");
  const std::string old_call = "(FunApp (StanLib " + function + " (FnLpmf " +
                               (propto ? "true" : "false") + ") AoS)";
  const size_t call = mir.find(old_call, log_prob);
  if (log_prob == std::string::npos || call == std::string::npos)
    throw std::logic_error("categorical UDF fixture has no density call");
  const std::string new_call = "(FunApp (UserDefined " + udf_name +
                               " (FnLpmf " + (propto ? "true" : "false") + "))";
  mir.replace(call, old_call.size(), new_call);
  return mir;
}

inline std::string categorical_udf_return_mir(const std::string& function) {
  std::string mir = categorical_check_mir(function, true, 1, 3, true, true);
  const std::string empty_functions = "(functions_block ())";
  const std::string functions = R"((functions_block
  (((fdrt (ReturnType UVector)) (fdname choose_vector) (fdsuffix FnPlain)
    (fdargs ((AutoDiffable ignored UVector) (DataOnly value UVector)))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Return
             (((pattern (Var value))
               (meta ((type_ UVector) (adlevel DataOnly)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>)))))";
  mir.replace(mir.find(empty_functions), empty_functions.size(), functions);

  const size_t density = mir.find("(FunApp (StanLib " + function);
  const std::string old_arg =
      "((pattern (Var local_arg))\n"
      "           (meta ((type_ UVector) (adlevel AutoDiffable))))";
  const size_t arg = mir.find(old_arg, density);
  if (density == std::string::npos || arg == std::string::npos)
    throw std::logic_error("categorical return fixture has no vector argument");
  const std::string new_arg = R"(((pattern
           (FunApp (UserDefined choose_vector FnPlain)
            (((pattern (Var local_arg))
              (meta ((type_ UVector) (adlevel AutoDiffable))))
             ((pattern (Var arg))
              (meta ((type_ UVector) (adlevel DataOnly)))))))
          (meta ((type_ UVector) (adlevel AutoDiffable)))))";
  mir.replace(arg, old_arg.size(), new_arg);
  return mir;
}

inline std::string categorical_udf_ternary_type_mir(
    const std::string& function) {
  std::string mir = categorical_udf_mir(function, true, false);
  const std::string formal = "(AutoDiffable p UVector)))";
  const size_t formal_at = mir.find(formal);
  if (formal_at == std::string::npos)
    throw std::logic_error("categorical UDF fixture has no vector formal");
  mir.replace(formal_at, formal.size(),
              "(AutoDiffable p UVector) (AutoDiffable ignored UReal)))");

  const size_t body_density = mir.find("(FunApp (StanLib " + function);
  const std::string body_arg =
      "((pattern (Var p))\n"
      "                   (meta ((type_ UVector) (adlevel AutoDiffable))))";
  const size_t body_arg_at = mir.find(body_arg, body_density);
  if (body_density == std::string::npos || body_arg_at == std::string::npos)
    throw std::logic_error("categorical UDF fixture has no body argument");
  const std::string ternary = R"(((pattern
                   (TernaryIf
                    ((pattern (Lit Int 1))
                     (meta ((type_ UInt) (adlevel DataOnly))))
                    ((pattern (Var p))
                     (meta ((type_ UVector) (adlevel AutoDiffable))))
                    ((pattern (Var p))
                     (meta ((type_ UVector) (adlevel AutoDiffable))))))
                  (meta ((type_ UVector) (adlevel AutoDiffable)))))";
  mir.replace(body_arg_at, body_arg.size(), ternary);

  const size_t log_prob = mir.find("(log_prob");
  const std::string caller_arg =
      "((pattern (Var arg))\n"
      "           (meta ((type_ UVector) (adlevel DataOnly))))";
  const size_t caller_arg_at = mir.find(caller_arg, log_prob);
  if (log_prob == std::string::npos || caller_arg_at == std::string::npos)
    throw std::logic_error("categorical UDF fixture has no caller argument");
  const std::string with_ignored =
      caller_arg +
      "\n          ((pattern (Var anchor))\n"
      "           (meta ((type_ UReal) (adlevel AutoDiffable))))";
  mir.replace(caller_arg_at, caller_arg.size(), with_ignored);
  return mir;
}

inline std::string categorical_udf_promoted_actual_mir(
    const std::string& function) {
  std::string mir = categorical_udf_ternary_type_mir(function);
  const std::string actual =
      "((pattern (Var anchor))\n"
      "           (meta ((type_ UReal) (adlevel AutoDiffable))))";
  const size_t log_prob = mir.find("(log_prob");
  const size_t actual_at = mir.find(actual, log_prob);
  if (log_prob == std::string::npos || actual_at == std::string::npos)
    throw std::logic_error("categorical UDF fixture has no real actual");
  const std::string promoted = R"(((pattern
           (Promotion
            ((pattern (Var outcome))
             (meta ((type_ UInt) (adlevel DataOnly))))
            UReal DataOnly))
          (meta ((type_ UReal) (adlevel DataOnly)))))";
  mir.replace(actual_at, actual.size(), promoted);
  return mir;
}

inline std::string categorical_promoted_ternary_mir(const std::string& function,
                                                    bool through_udf) {
  std::string mir =
      through_udf ? categorical_udf_mir(function, true, true)
                  : categorical_check_mir(function, true, 1, 3, true, true);
  const std::string inputs = "(input_vars\n  ((outcome";
  const size_t input = mir.find(inputs);
  if (input == std::string::npos)
    throw std::logic_error("categorical ternary fixture has no inputs");
  mir.replace(input, inputs.size(),
              "(input_vars\n  ((flag <opaque> SInt)\n   (outcome");

  const size_t log_prob = mir.find("(log_prob");
  const std::string old_arg =
      "((pattern (Var local_arg))\n"
      "           (meta ((type_ UVector) (adlevel AutoDiffable))))";
  const size_t arg = mir.find(old_arg, log_prob);
  if (log_prob == std::string::npos || arg == std::string::npos)
    throw std::logic_error(
        "categorical ternary fixture has no vector argument");
  const std::string ternary = R"(((pattern
           (TernaryIf
            ((pattern (Var flag))
             (meta ((type_ UInt) (adlevel DataOnly))))
            ((pattern (Var arg))
             (meta ((type_ UVector) (adlevel DataOnly))))
            ((pattern (Var local_arg))
             (meta ((type_ UVector) (adlevel AutoDiffable))))))
          (meta ((type_ UVector) (adlevel AutoDiffable)))))";
  mir.replace(arg, old_arg.size(), ternary);
  return mir;
}

inline std::string categorical_parameter_effect_mir(
    std::string mir, const std::string& function) {
  const std::string empty_functions = "(functions_block ())";
  const std::string functions = R"((functions_block
  (((fdrt (ReturnType UInt)) (fdname noisy) (fdsuffix FnPlain)
    (fdargs ((DataOnly x UInt)))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (NRFunApp (CompilerInternal FnPrint)
             (((pattern (Lit Str "categorical effect"))
               (meta ((type_ UReal) (adlevel DataOnly)))))))
           (meta <opaque>))
          ((pattern
            (Return
             (((pattern (Var x))
               (meta ((type_ UInt) (adlevel DataOnly)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>)))))";
  mir.replace(mir.find(empty_functions), empty_functions.size(), functions);

  const size_t density =
      mir.find("(FunApp (StanLib " + function, mir.find("(log_prob"));
  const std::string old_outcome =
      "((pattern (Var y)) (meta ((type_ UInt) (loc <opaque>) (adlevel "
      "DataOnly))))";
  const size_t outcome = mir.find(old_outcome, density);
  if (density == std::string::npos || outcome == std::string::npos)
    throw std::logic_error("categorical effect fixture has no scalar outcome");
  const std::string new_outcome = R"(((pattern
                (FunApp (UserDefined noisy FnPlain)
                 (((pattern (Var y))
                   (meta ((type_ UInt) (adlevel DataOnly)))))))
               (meta ((type_ UInt) (adlevel DataOnly)))))";
  mir.replace(outcome, old_outcome.size(), new_outcome);
  return mir;
}

inline std::string categorical_write_array_mir(
    std::string mir, const std::string& function, bool propto,
    bool island_arg = false, bool data_udf = false, bool force_interp = false,
    int forced_outcome_count = 1) {
  if (data_udf) {
    const std::string empty_functions = "(functions_block ())";
    const std::string functions = R"((functions_block
  (((fdrt (ReturnType UReal)) (fdname data_categorical) (fdsuffix FnPlain)
    (fdargs ((DataOnly y UInt) (DataOnly p UVector)))
    (fdbody
     (((pattern
        (Block
         (((pattern
            (Return
             (((pattern (Lit Real 0.0))
               (meta ((type_ UReal) (adlevel DataOnly)))))))
           (meta <opaque>)))))
       (meta <opaque>))))
    (fdloc <opaque>)))))";
    mir.replace(mir.find(empty_functions), empty_functions.size(), functions);
  }
  const std::string boundary = "    (meta <opaque>))))\n (transform_inits";
  const size_t pos = mir.find(boundary, mir.find("(generate_quantities"));
  if (pos == std::string::npos)
    throw std::logic_error("categorical write-array fixture has no boundary");
  const std::string vector_arg = island_arg ? R"(((pattern
              (TernaryIf
               ((pattern
                 (FunApp (StanLib Greater__ FnPlain AoS)
                  (((pattern
                     (Indexed
                      ((pattern (Var theta))
                       (meta ((type_ UVector) (adlevel AutoDiffable))))
                      ((Single
                        ((pattern (Lit Int 1))
                         (meta ((type_ UInt) (adlevel DataOnly))))))))
                    (meta ((type_ UReal) (adlevel AutoDiffable))))
                   ((pattern (Lit Int 0))
                    (meta ((type_ UInt) (adlevel DataOnly)))))))
                (meta ((type_ UInt) (adlevel AutoDiffable))))
               ((pattern (Var theta))
                (meta ((type_ UVector) (adlevel AutoDiffable))))
               ((pattern (Var theta))
                (meta ((type_ UVector) (adlevel AutoDiffable))))))
             (meta ((type_ UVector) (adlevel DataOnly)))))"
                                            : R"(((pattern (Var theta))
             (meta ((type_ UVector) (adlevel DataOnly)))))";
  std::ostringstream replacement;
  replacement
      << "    (meta <opaque>))\n"
      << "   ((pattern\n"
      << "     (Decl (decl_adtype DataOnly) (decl_id categorical_value)\n"
      << "      (decl_type (Sized SReal))\n"
      << "      (initialize\n"
      << "       (Assign\n"
      << "        ((pattern\n"
      << "          (FunApp "
      << (data_udf ? "(UserDefined data_categorical FnPlain)"
                   : "(StanLib " + function + " (FnLpmf " +
                         (propto ? std::string("true") : std::string("false")) +
                         ") AoS)")
      << "\n"
      << "           (";
  if (force_interp) {
    const std::string draw = R"(((pattern
             (FunApp (StanLib binomial_rng FnRng AoS)
              (((pattern (Lit Int 1))
                (meta ((type_ UInt) (adlevel DataOnly))))
               ((pattern (Lit Real 1.0))
                (meta ((type_ UReal) (adlevel DataOnly)))))))
            (meta ((type_ UInt) (adlevel DataOnly)))))";
    if (forced_outcome_count == 1) {
      replacement << draw;
    } else {
      replacement << "((pattern\n"
                  << "             (FunApp (CompilerInternal FnMakeArray)\n"
                  << "              (";
      for (int i = 0; i < forced_outcome_count; ++i) replacement << draw;
      replacement << ")))\n"
                  << "            (meta ((type_ (UArray UInt)) (adlevel "
                     "DataOnly))))";
    }
  } else {
    replacement << "((pattern (Var y))\n"
                << "             (meta ((type_ UInt) (adlevel DataOnly))))";
  }
  replacement << "\n            " << vector_arg << ")))\n"
              << "         (meta ((type_ UReal) (adlevel DataOnly))))))))\n"
              << "    (meta <opaque>))\n"
              << "   ((pattern\n"
              << "     (NRFunApp\n"
              << "      (CompilerInternal\n"
              << "       (FnWriteParam (unconstrain_opt ())\n"
              << "        (var\n"
              << "         ((pattern (Var categorical_value))\n"
              << "          (meta ((type_ UReal) (adlevel DataOnly)))))))\n"
              << "      ()))\n"
              << "    (meta <opaque>))))\n"
              << " (transform_inits";
  mir.replace(pos, boundary.size(), replacement.str());
  return mir;
}

inline std::string categorical_transformed_data_mismatch_mir(bool in_gq) {
  std::string mir = categorical_check_mir("categorical_lpmf", true, 1, 3);
  const std::string inputs = "(input_vars\n  ((outcome";
  mir.replace(mir.find(inputs), inputs.size(),
              "(input_vars\n  ((source <opaque> SReal)\n   (outcome");
  const std::string empty_prepare = "(prepare_data ())";
  const std::string prepare = R"((prepare_data
  (((pattern
     (Decl (decl_adtype DataOnly) (decl_id td_outcome)
      (decl_type (Sized SReal))
      (initialize
       (Assign
        ((pattern (Var source))
         (meta ((type_ UReal) (adlevel DataOnly))))))))
    (meta <opaque>)))))";
  mir.replace(mir.find(empty_prepare), empty_prepare.size(), prepare);

  if (!in_gq) {
    const size_t call = mir.find("(FunApp (StanLib categorical_lpmf");
    const std::string needle = "(Var outcome)";
    const size_t outcome = mir.find(needle, call);
    if (call == std::string::npos || outcome == std::string::npos)
      throw std::logic_error("categorical test fixture has no outcome call");
    mir.replace(outcome, needle.size(), "(Var td_outcome)");
    return mir;
  }

  while (!mir.empty() && (mir.back() == '\n' || mir.back() == '\r'))
    mir.pop_back();
  mir.pop_back();  // root list
  mir += R"(
 (generate_quantities
  (((pattern
     (TargetPE
      ((pattern
        (FunApp (StanLib categorical_lpmf (FnLpmf true) AoS)
         (((pattern (Var td_outcome))
           (meta ((type_ UInt) (adlevel DataOnly))))
          ((pattern (Var arg))
           (meta ((type_ UVector) (adlevel DataOnly)))))))
       (meta ((type_ UReal) (adlevel DataOnly))))))
    (meta <opaque>)))))
)";
  return mir;
}

#endif
