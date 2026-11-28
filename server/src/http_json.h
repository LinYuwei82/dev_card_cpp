#pragma once

// HTTP 层的通用 JSON / 请求参数助手：响应包装、DTO 序列化、查询参数解析。
// 只做格式转换，不承载业务规则（业务在 core/service）。

#include <cstddef>
#include <cstdint>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "devcard/models.h"
#include "devcard/service/device_service.h"

namespace devcard::detail {

nlohmann::json parse_body(const std::string& body);

void set_json(httplib::Response& response, const std::string& body,
              int status = 200);
void set_json(httplib::Response& response, const nlohmann::json& value,
              int status = 200);
void set_asset(httplib::Response& response, const std::string& body,
               const char* type);

nlohmann::json device_json(const Device& device);
nlohmann::json conflict_json(const ConflictError& error);
nlohmann::json user_json(const User& user);
Device device_from_json(const nlohmann::json& body);

int int_param(const httplib::Request& request, const char* name, int fallback);
DeviceQuery query_from(const httplib::Request& request);

std::string trim_copy(const std::string& value);
std::size_t utf8_length(const std::string& value);

// 路径里的 id 只由正则保证是数字，长度不受限：std::stoll 会抛 out_of_range，
// 落到 std::exception 分支就成了 500，还会把 "stoll" 原样透给客户端。
std::int64_t parse_path_id(const std::string& text);

}  // namespace devcard::detail
