// The testable core of the BridgeStan facade.
//
// bs_model_construct unpacks the manifest embedded in its data argument
// and then builds a model. This header is the seam under that:
// bs_model_from_mir is the whole facade minus the unpacking, and
// bs_read_manifest is the manifest check on its own.
//
// Not installed as part of the public C ABI (runtime/include ships it, but
// bridgestan.h is the interface clients bind); it exists for this
// project's own tests and for the construction path in bridgestan_abi.cpp.
#ifndef STANLI_BRIDGESTAN_INTERNAL_HPP
#define STANLI_BRIDGESTAN_INTERNAL_HPP

#include <string>

class bs_model;

// Build a model from transformed-MIR text directly. `data` follows the
// BridgeStan convention: null or empty for no data, a path ending in ".json",
// or a JSON literal. `seed` seeds RNG calls in transformed data. Returns null
// on failure with a heap-allocated message in *error_msg, which the caller
// frees with bs_free_error_msg.
bs_model* bs_model_from_mir(const char* mir, const char* data,
                            unsigned int seed, char** error_msg,
                            const char* name = "stanli_model");

namespace stanli {

// This runtime binary's build id -- the same string stanli_build_id()
// returns, read from the same compile definition. Taken from the macro
// rather than through the C ABI because capi.cpp is not part of the static
// library, and the facade has to work in both.
const char* bs_build_id();

// The manifest's shape: {"build_id": ..., "name": ..., "mir": ...}.
struct BsManifest {
  std::string build_id;
  std::string name;
  std::string mir;
};

// Parse a manifest and check it was written for THIS runtime binary.
// Returns false with a message in *err on malformed JSON, a missing field,
// or a build id that is not bs_build_id() -- a stale pair must fail loudly
// rather than lower MIR that this binary no longer understands.
bool bs_read_manifest(const std::string& text, BsManifest* out,
                      std::string* err);

}  // namespace stanli

#endif
