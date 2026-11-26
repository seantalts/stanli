// rapidjson, with its assertions turned into exceptions. Include rapidjson
// only through this header: every translation unit must see the same
// RAPIDJSON_ASSERT, or the inline functions that use it differ between
// objects.
//
// rapidjson asserts on some malformed input instead of reporting a parse
// error. "Infe1" fails `length >= 1` in StrtodFullPrecision: with the
// assertion compiled in the process aborts (as CmdStan does), and without
// it the number reads as 0. Throwing lets the data readers report a parse
// error in every build.
#pragma once

#ifdef RAPIDJSON_RAPIDJSON_H_
#error "rapidjson was included before runtime/src/rapidjson.hpp"
#endif

#include <stdexcept>
#include <string>

namespace stanli {

struct RapidjsonAssertion : std::runtime_error {
  using std::runtime_error::runtime_error;
};

}  // namespace stanli

// RAPIDJSON_ASSERT_THROWS makes rapidjson leave out the checks inside its
// noexcept functions, where a throw would call std::terminate.
#define RAPIDJSON_ASSERT_THROWS
#define RAPIDJSON_ASSERT(x)                  \
  ((x) ? static_cast<void>(0)                \
       : throw ::stanli::RapidjsonAssertion( \
             std::string("rapidjson assertion failed: ") + #x))

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/filereadstream.h>
#include <rapidjson/memorystream.h>
#include <rapidjson/reader.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
