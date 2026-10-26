#include <stan/math/rev/fun/mdivide_left_tri.hpp>
#include <cstdio>
#include <limits>

template <Eigen::UpLoType Triangle, int Columns>
void probe(int n, int k, bool alias) {
  using stan::math::var;
  stan::math::nested_rev_autodiff scope;
  Eigen::Matrix<var, -1, -1> a(n, n);
  Eigen::Matrix<var, -1, Columns> b(n, k);
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i)
      a(i, j) = i == j ? 2.0 + i * 0.125
                       : 0.125 * ((i + 2 * j) % 5 - 2);
  for (int j = 0; j < k; ++j)
    for (int i = 0; i < n; ++i)
      b(i, j) = alias ? a(i, j) : var(0.125 * (i - 3 * j));
  const auto c = stan::math::mdivide_left_tri<Triangle>(a, b);
  var objective = 0;
  for (int j = 0; j < k; ++j)
    for (int i = 0; i < n; ++i)
      objective += c(i, j) * (0.125 * (1 + i - 2 * j));
  // Existing contributions must survive the solve's reverse pass too.
  for (int i = 0; i < a.size(); ++i) objective += a.data()[i] * 0.25;
  for (int i = 0; i < b.size(); ++i) objective += b.data()[i] * -0.125;
  stan::math::grad(objective.vi_);
  std::printf("case %d %d %d %d %d\n", Triangle, Columns, n, k, alias);
  for (int i = 0; i < c.size(); ++i) std::printf("c %a\n", c.data()[i].val());
  for (int i = 0; i < a.size(); ++i) std::printf("a %a\n", a.data()[i].adj());
  for (int i = 0; i < b.size(); ++i) std::printf("b %a\n", b.data()[i].adj());
}

template <Eigen::UpLoType Triangle>
void cases() {
  for (int n : {0, 1, 2, 5, 17, 33}) {
    probe<Triangle, 1>(n, 1, false);
    probe<Triangle, -1>(n, 1, false);
    probe<Triangle, -1>(n, 3, false);
    probe<Triangle, -1>(n, n, true);
  }
}

int main() {
  cases<Eigen::Lower>();
  cases<Eigen::Upper>();
}
