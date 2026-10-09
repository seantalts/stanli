#include <boost/multiprecision/cpp_bin_float.hpp>

using mp = boost::multiprecision::number<
    boost::multiprecision::cpp_bin_float<50>, boost::multiprecision::et_off>;

#include <stanli/mir_interp.hpp>

template class stanli::MirInterp<mp>;

int main() {}
