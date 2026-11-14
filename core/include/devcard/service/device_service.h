#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "devcard/dao/device_dao.h"
#include "devcard/db.h"
#include "devcard/models.h"

namespace devcard {

struct DevicePage {
  std::vector<Device> items;
  std::int64_t total = 0;
  int page = 0;
  int page_size = 20;
};

struct DeviceOptions {
  std::vector<std::string> names;
  std::vector<std::string> locations;
};

// 台账业务规则：校验、手工新增三字段查重、分页边界。
class DeviceService {
 public:
  explicit DeviceService(Db& db) : db_(db) {}

  DevicePage list(const DeviceQuery& query) const;
  Device get(std::int64_t id) const;
  Device create(Device device);
  Device update(std::int64_t id, Device device);
  void remove(std::int64_t id);
  DeviceOptions options() const;

 private:
  void validate(Device& device) const;

  Db& db_;
  DeviceDao dao_;
};

}  // namespace devcard
