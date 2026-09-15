#include <stanli/data.hpp>

#include <stan/io/json/json_data_handler.hpp>
#include <rapidjson/filereadstream.h>
#include <rapidjson/memorystream.h>

#include <cstdio>
#include <memory>
#include <sstream>

namespace stanli {
namespace {

// Use CmdStan's handler and parser flags, but feed RapidJSON directly instead
// of paying an istream call per character. The handler owns the column-major
// arrays; move them into DataMap rather than copying through var_context.
template <typename Stream>
DataMap read_json(Stream& input) {
  stan::json::vars_map_r reals;
  stan::json::vars_map_i integers;
  stan::json::json_data_handler handler(reals, integers);
  stan::json::RapidJSONHandler<stan::json::json_data_handler> filter(handler);
  rapidjson::Reader reader;
  handler.start_text();
  if (!reader.Parse<rapidjson::kParseNanAndInfFlag |
                    rapidjson::kParseValidateEncodingFlag |
                    rapidjson::kParseFullPrecisionFlag>(input, filter)) {
    std::ostringstream error;
    error << "Error in JSON parsing at offset " << reader.GetErrorOffset()
          << ": "
          << (filter.error_message_.empty()
                  ? rapidjson::GetParseError_En(reader.GetParseErrorCode())
                  : filter.error_message_);
    throw stan::json::json_error(error.str());
  }
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
}  // namespace

DataMap DataMap::from_json(const std::string& text) {
  rapidjson::MemoryStream input(text.data(), text.size());
  return read_json(input);
}

DataMap DataMap::from_json_file(const std::string& path) {
  std::unique_ptr<std::FILE, decltype(&std::fclose)> file(
      std::fopen(path.c_str(), "rb"), &std::fclose);
  if (!file) throw std::runtime_error("data: cannot open " + path);
  char buffer[65536];
  rapidjson::FileReadStream input(file.get(), buffer, sizeof(buffer));
  return read_json(input);
}

}  // namespace stanli
