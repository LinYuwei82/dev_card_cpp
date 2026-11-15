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
  // 乐观锁版本：读接口下发，编辑提交时带回；0 表示调用方没有版本
  // （HTTP 层会拒绝，避免退化成盲写）。
  std::int64_t version = 0;
  std::string updated_by;
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
