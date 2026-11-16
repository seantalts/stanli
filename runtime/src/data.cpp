#include <stanli/data.hpp>

#include <stanli/compile.hpp>

#include "data_document.hpp"

#include <stan/io/json/json_data_handler.hpp>
#include <rapidjson/filereadstream.h>
#include <rapidjson/memorystream.h>

#include <cstdio>
#include <memory>
#include <sstream>

namespace stanli {
namespace {

constexpr unsigned kDataParseFlags = rapidjson::kParseNanAndInfFlag |
                                     rapidjson::kParseValidateEncodingFlag |
                                     rapidjson::kParseFullPrecisionFlag;

using JsonFilter = stan::json::RapidJSONHandler<stan::json::json_data_handler>;

std::string parse_error_message(size_t offset, rapidjson::ParseErrorCode code,
                                const std::string& handler_message) {
  std::ostringstream error;
  error << "Error in JSON parsing at offset " << offset << ": "
        << (handler_message.empty() ? rapidjson::GetParseError_En(code)
                                    : handler_message);
  return error.str();
}

// Use CmdStan's handler and parser flags, but feed RapidJSON directly instead
// of paying an istream call per character. The handler owns the column-major
// arrays; move them into DataMap rather than copying through var_context.
// `feed` drives the handler's events and returns an error message, empty on
// success.
template <typename Feed>
DataMap read_json(Feed feed) {
  stan::json::vars_map_r reals;
  stan::json::vars_map_i integers;
  stan::json::json_data_handler handler(reals, integers);
  JsonFilter filter(handler);
  handler.start_text();
  const std::string error = feed(filter);
  if (!error.empty()) throw stan::json::json_error(error);
  handler.end_text();
  DataMap data;
  for (auto& [name, value] : reals) {
    if (value.second.empty())
      data.set_real(name, value.first.at(0));
    else
      data.set_real_array(name, std::move(value.first),
                          {value.second.begin(), value.second.end()});
  }
  for (auto& [name, value] : integers) {
    if (value.second.empty())
      data.set_int(name, value.first.at(0));
    else
      data.set_int_array(name, std::move(value.first),
                         {value.second.begin(), value.second.end()});
  }
  return data;
}

template <typename Stream>
DataMap read_json_stream(Stream& input) {
  return read_json([&](JsonFilter& filter) {
    rapidjson::Reader reader;
    if (reader.Parse<kDataParseFlags>(input, filter)) return std::string();
    return parse_error_message(reader.GetErrorOffset(),
                               reader.GetParseErrorCode(),
                               filter.error_message_);
  });
}
}  // namespace

DataMap DataMap::from_json(const std::string& text) {
  rapidjson::MemoryStream input(text.data(), text.size());
  return read_json_stream(input);
}

DataMap DataMap::from_json_file(const std::string& path) {
  std::unique_ptr<std::FILE, decltype(&std::fclose)> file(
      std::fopen(path.c_str(), "rb"), &std::fclose);
  if (!file) throw std::runtime_error("data: cannot open " + path);
  char buffer[65536];
  rapidjson::FileReadStream input(file.get(), buffer, sizeof(buffer));
  return read_json_stream(input);
}

rapidjson::Document parse_data_document(const std::string& text) {
  rapidjson::Document doc;
  doc.Parse<kDataParseFlags>(text.data(), text.size());
  if (doc.HasParseError())
    throw stan::json::json_error(parse_error_message(
        doc.GetErrorOffset(), doc.GetParseError(), std::string()));
  return doc;
}

DataMap data_from_document(const rapidjson::Value& object) {
  return read_json([&](JsonFilter& filter) {
    if (object.Accept(filter)) return std::string();
    return filter.error_message_.empty()
               ? std::string("Error in JSON data: the handler refused it")
               : filter.error_message_;
  });
}

void unconstrain_inits_json(const CompiledModel& cm, const char* json,
                            double* out) {
  const DataMap supplied = DataMap::from_json(json == nullptr ? "{}" : json);
  unconstrain_inits(cm,
                    std::map<std::string, DataMap::Entry>(
                        supplied.entries().begin(), supplied.entries().end()),
                    out);
}

}  // namespace stanli
