// Data JSON as a parsed document, for a caller that must take something out
// of the object before the rest is read as data (the BridgeStan facade's
// "__stanli" member).
#pragma once

#include <stanli/data.hpp>

#include "rapidjson.hpp"

#include <string>

namespace stanli {

// Parses with the flags DataMap::from_json uses: bare NaN and Infinity,
// full-precision doubles, validated UTF-8. Throws stan::json::json_error.
rapidjson::Document parse_data_document(const std::string& text);

// Reads a parsed object exactly as DataMap::from_json reads text.
DataMap data_from_document(const rapidjson::Value& object);

}  // namespace stanli
