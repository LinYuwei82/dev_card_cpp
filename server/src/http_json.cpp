#include "http_json.h"

#include <string>

#include "devcard/error.h"

namespace devcard::detail {
namespace {

using nlohmann::json;

std::string json_body(const json& value) { return value.dump(); }

}  // namespace

void set_json(httplib::Response& response, const std::string& body,
              int status) {
  response.status = status;
  response.set_header("Content-Type", "application/json; charset=utf-8");
  response.set_header("Cache-Control", "no-store");
  response.body = body;
}

void set_json(httplib::Response& response, const json& value, int status) {
  set_json(response, json_body(value), status);
}

void set_asset(httplib::Response& response, const std::string& body,
               const char* type) {
  response.status = 200;
  response.set_header("Content-Type", type);
  response.set_header("Cache-Control", "no-store");
  response.body = body;
}

json device_json(const Device& device) {
  return {{"id", device.id},
          {"dev_name", device.dev_name},
          {"location", device.location},
          {"control_range", device.control_range},
          {"phone", device.phone},
          {"created_at", device.created_at},
          {"updated_at", device.updated_at},
          {"version", device.version},
          {"updated_by", device.updated_by}};
}

// 409 响应体：最新快照 + 冲突字段 + 版本号。
// 前端据此提示「刷新后重填」；P3 若以后开放管理员强制覆盖，
// 直接复用这里的 current/version 即可，不需要再改接口形状。
json conflict_json(const ConflictError& error) {
  const ConflictSnapshot& snapshot = error.snapshot();
  return {{"error", "conflict"},
          {"message", error.what()},
          {"fields", snapshot.fields},
          {"current", device_json(snapshot.current)},
          {"current_version", snapshot.current.version},
          {"updated_by", snapshot.current.updated_by},
          {"updated_at", snapshot.current.updated_at}};
}

json user_json(const User& user) {
  return {{"user_name", user.user_name},
          {"display_name", user.display_name},
          {"role", user.role},
          {"must_change_password", user.must_change_password},
          {"locked_until", user.locked_until},
          {"created_at", user.created_at},
          {"updated_at", user.updated_at}};
}

Device device_from_json(const json& body) {
  Device device;
  device.dev_name = body.value("dev_name", "");
  device.location = body.value("location", "");
  device.control_range = body.value("control_range", "");
  device.phone = body.value("phone", "");
  device.version = body.value("version", static_cast<std::int64_t>(0));
  return device;
}

json parse_body(const std::string& body) {
  try {
    return json::parse(body.empty() ? "{}" : body);
  } catch (const json::exception&) {
    throw AppError("bad_json", "请求体不是合法 JSON");
  }
}

int int_param(const httplib::Request& request, const char* name, int fallback) {
  const std::string value = request.get_param_value(name);
  if (value.empty()) return fallback;
  try {
    return std::stoi(value);
  } catch (...) {
    return fallback;
  }
}

DeviceQuery query_from(const httplib::Request& request) {
  DeviceQuery query;
  query.keyword = request.get_param_value("keyword");
  query.field = request.get_param_value("field");
  query.name = request.get_param_value("name");
  query.location = request.get_param_value("location");
  query.page = int_param(request, "page", 0);
  query.page_size = int_param(request, "page_size", 20);
  query.sort = request.get_param_value("sort");
  if (query.sort.empty()) query.sort = "id";
  query.desc = request.get_param_value("desc") == "1";
  return query;
}

std::string trim_copy(const std::string& value) {
  const std::size_t begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const std::size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

// 按 UTF-8 起始字节计数，一个汉字/符号算一个字。
std::size_t utf8_length(const std::string& value) {
  std::size_t count = 0;
  for (unsigned char ch : value) {
    if ((ch & 0xC0) != 0x80) ++count;
  }
  return count;
}

std::int64_t parse_path_id(const std::string& text) {
  std::size_t consumed = 0;
  std::int64_t value = 0;
  try {
    value = std::stoll(text, &consumed);
  } catch (const std::exception&) {
    throw AppError("not_found", "记录不存在");
  }
  if (consumed != text.size()) {
    throw AppError("not_found", "记录不存在");
  }
  return value;
}

}  // namespace devcard::detail
