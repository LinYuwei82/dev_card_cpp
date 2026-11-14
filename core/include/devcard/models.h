#pragma once

#include <cstdint>
#include <string>

namespace devcard {

struct Device {
  std::int64_t id = 0;
  std::string dev_name;
  std::string location;
  std::string control_range;
  std::string phone;
  std::string created_at;
  std::string updated_at;
};

struct DeviceQuery {
  std::string keyword;
  std::string field;  // dev_name | location | control_range | phone；空 = 全字段
  std::string name;
  std::string location;
  int page = 0;
  int page_size = 200;  // 上限 1000
  std::string sort = "id";
  bool desc = false;
};

struct User {
  std::string user_name;
  std::string password_hash;
  std::string display_name;
  std::string role = "user";
  bool must_change_password = false;
  int failed_count = 0;
  std::string locked_until;
  std::string created_at;
  std::string updated_at;
};

}  // namespace devcard
