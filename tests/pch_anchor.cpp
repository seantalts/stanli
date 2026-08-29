// Nothing but an anchor for the shared precompiled header.
//
// CMake's REUSE_FROM needs a real target to build the .pch, and that target
// needs a source file. This is deliberately not tests/tbb_stub.cpp, which was
// the obvious candidate and is the wrong one: the stub's non-Apple branch
// DEFINES tbb::internal::task_scheduler_observer_v3 itself, and the
// precompiled header force-includes <stan/math.hpp>, which brings in the real
// TBB headers that already declare it. On macOS the stub takes an inline-asm
// branch instead and the collision never appears, so this only ever fails on
// Linux and Windows -- which is exactly how it reached CI.
namespace stanli {
namespace {
// An empty translation unit is legal but says nothing about why it is here.
[[maybe_unused]] constexpr int kPchAnchor = 0;
}  // namespace
}  // namespace stanli
