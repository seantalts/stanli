// CompilerInternal FnCheck semantics in the shared MIR interpreter.
//
// Generated Stan uses the same check statement during construction and draw
// evaluation. The relation comes from FnCheck's payload; value and bound
// shapes come from the MIR type plus the evaluated dimensions. These tests
// stay below lowering so malformed and container-bound cases can be isolated.
#include <stanli/mir_interp.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using stanli::DataMap;
using stanli::MirInterp;
using stanli::mir::Expr;
using stanli::mir::Stmt;
using stanli::mir::Transform;
using stanli::mir::UnsizedLeaf;

int failures = 0;

void check(bool ok, const char* what) {
  if (ok) return;
  ++failures;
  std::printf("FAIL %s\n", what);
}

Expr var(std::string name, UnsizedLeaf leaf, uint8_t depth = 0) {
  Expr e;
  e.kind = Expr::Var;
  e.name = std::move(name);
  e.unsized = {depth, leaf};
  return e;
}

Expr real(double x) {
  Expr e;
  e.kind = Expr::LitReal;
  e.lit = x;
  e.unsized.leaf = UnsizedLeaf::Real;
  return e;
}

Stmt fn_check(Transform::Kind relation, Expr value, Expr bound,
              std::string name = "x") {
  Stmt s;
  s.kind = Stmt::NRFunApp;
  s.fn_name = "FnCheck";
  s.check_var_name = std::move(name);
  Transform t;
  t.kind = relation;
  t.args.push_back(bound);
  s.check_transform = std::move(t);
  s.fn_args = {std::move(value), std::move(bound)};
  return s;
}

DataMap::Entry values(std::vector<double> x, std::vector<int64_t> dims) {
  DataMap::Entry e;
  e.r = std::move(x);
  e.dims = std::move(dims);
  return e;
}

MirInterp<double> interp() {
  static const std::map<std::string, const stanli::mir::FunDef*> no_funs;
  return MirInterp<double>(no_funs, "FnCheck test");
}

}  // namespace

int main() {
  // Pairwise vector bounds are legal and inclusive. A later bad element must
  // reject with the same domain-error class as Stan Math.
  {
    auto m = interp();
    m.env()["x"] = values({1.0, 3.0}, {2});
    m.env()["lo"] = values({1.0, 2.0}, {2});
    const Stmt s = fn_check(Transform::Lower, var("x", UnsizedLeaf::Vector),
                            var("lo", UnsizedLeaf::Vector));
    bool accepted = true;
    try {
      m.exec(s);
    } catch (const std::exception&) {
      accepted = false;
    }
    check(accepted, "conformable vector lower bound is accepted");

    m.env()["x"] = values({1.0, 1.5}, {2});
    bool domain = false;
    try {
      m.exec(s);
    } catch (const std::domain_error& e) {
      domain = std::string(e.what()).find("x") != std::string::npos;
    } catch (const std::exception&) {
    }
    check(domain, "later pairwise lower violation is a named domain error");
  }

  // Equal flat widths do not make logical shapes interchangeable.
  {
    auto m = interp();
    m.env()["x"] = values({1.0, 2.0}, {2});
    m.env()["hi"] = values({3.0, 3.0}, {2});
    const Stmt s = fn_check(Transform::Upper, var("x", UnsizedLeaf::Vector),
                            var("hi", UnsizedLeaf::RowVector));
    bool mismatch = false;
    try {
      m.exec(s);
    } catch (const std::invalid_argument&) {
      mismatch = true;
    } catch (const std::exception&) {
    }
    check(mismatch, "vector and row-vector bounds do not alias");
  }
  {
    auto m = interp();
    m.env()["x"] = values({1.0, 2.0}, {1, 2});
    m.env()["hi"] = values({3.0, 3.0}, {2, 1});
    const Stmt s = fn_check(Transform::Upper, var("x", UnsizedLeaf::Matrix),
                            var("hi", UnsizedLeaf::Matrix));
    bool mismatch = false;
    try {
      m.exec(s);
    } catch (const std::invalid_argument&) {
      mismatch = true;
    } catch (const std::exception&) {
    }
    check(mismatch, "matrix bounds require identical dimensions");
  }

  // A one-element vector remains a container. It may pair with another
  // vector, but it is not a scalar bound for a scalar value.
  {
    auto m = interp();
    m.env()["x"] = values({1.0}, {1});
    m.env()["lo"] = values({1.0}, {1});
    const Stmt vectors =
        fn_check(Transform::Lower, var("x", UnsizedLeaf::Vector),
                 var("lo", UnsizedLeaf::Vector));
    bool accepted = true;
    try {
      m.exec(vectors);
    } catch (const std::exception&) {
      accepted = false;
    }
    check(accepted, "length-one vector bounds remain pairwise containers");

    m.env()["s"] = values({1.0}, {});
    const Stmt scalar = fn_check(Transform::Lower, var("s", UnsizedLeaf::Real),
                                 var("lo", UnsizedLeaf::Vector), "s");
    bool mismatch = false;
    try {
      m.exec(scalar);
    } catch (const std::invalid_argument&) {
      mismatch = true;
    } catch (const std::exception&) {
    }
    check(mismatch, "scalar value refuses a length-one container bound");
  }

  // Stan's checks compare with !(x >= b) / !(x <= b): NaN rejects when a
  // value is visited, while an empty container performs no comparisons.
  {
    auto m = interp();
    m.env()["x"] = values({}, {0});
    const Stmt empty = fn_check(Transform::Lower, var("x", UnsizedLeaf::Vector),
                                real(std::numeric_limits<double>::quiet_NaN()));
    bool accepted = true;
    try {
      m.exec(empty);
    } catch (const std::exception&) {
      accepted = false;
    }
    check(accepted, "empty container check is vacuous with a NaN bound");

    m.env()["x"] = values({0.0}, {1});
    bool domain = false;
    try {
      m.exec(empty);
    } catch (const std::domain_error&) {
      domain = true;
    }
    check(domain, "nonempty container rejects a NaN bound");
  }

  if (failures == 0) std::printf("test_mir_checks OK\n");
  return failures == 0 ? 0 : 1;
}
