#include "devcard/service/device_service.h"

#include <algorithm>
#include <cctype>

#include "devcard/error.h"

namespace devcard {
namespace {

std::string trim(std::string value) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(),
              std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
              value.end());
  return value;
}

std::size_t utf8_length(const std::string& value) {
  std::size_t count = 0;
  for (unsigned char c : value) {
    if ((c & 0xC0) != 0x80) ++count;
  }
  return count;
}

void require_text(const std::string& value, const char* label) {
  if (value.empty()) throw AppError("invalid_device", std::string(label) + "不能为空");
  if (utf8_length(value) > 20)
    throw AppError("invalid_device", std::string(label) + "不能超过 20 个字符");
}

}  // namespace

void DeviceService::validate(Device& device) const {
  device.dev_name = trim(device.dev_name);
  device.location = trim(device.location);
  device.control_range = trim(device.control_range);
  device.phone = trim(device.phone);
  require_text(device.dev_name, "设备名称");
  require_text(device.location, "位置");
  require_text(device.control_range, "控制范围");
  require_text(device.phone, "维护电话");
}

DevicePage DeviceService::list(const DeviceQuery& query) const {
  DeviceQuery normalized = query;
  normalized.page = std::max(0, normalized.page);
  normalized.page_size = std::clamp(normalized.page_size, 1, 1000);

  DevicePage result;
  result.page = normalized.page;
  result.page_size = normalized.page_size;
  result.items = dao_.list(db_, normalized);
  result.total = dao_.count(db_, normalized);
  return result;
}

Device DeviceService::get(std::int64_t id) const {
  Device device;
  if (!dao_.find(db_, id, device))
    throw AppError("not_found", "设备不存在");
  return device;
}

Device DeviceService::create(Device device) {
  validate(device);
  db_.write([&](sqlite3* conn) {
    if (dao_.exists_triplet(conn, device.dev_name, device.location,
                            device.control_range)) {
      throw AppError("duplicate", "你要添加的设备己经存在，请重新输入");
    }
    dao_.insert(conn, device);
  });
  return device;
}

Device DeviceService::update(std::int64_t id, Device device) {
  validate(device);
  device.id = id;
  db_.write([&](sqlite3* conn) {
    dao_.update(conn, device);
    if (sqlite3_changes(conn) == 0)
      throw AppError("not_found", "设备不存在");
  });
  Device saved;
  if (!dao_.find(db_, id, saved)) throw AppError("not_found", "设备不存在");
  return saved;
}

void DeviceService::remove(std::int64_t id) {
  db_.write([&](sqlite3* conn) {
    dao_.erase(conn, id);
    if (sqlite3_changes(conn) == 0)
      throw AppError("not_found", "设备不存在");
  });
}

DeviceOptions DeviceService::options() const {
  return {dao_.distinct_names(db_), dao_.distinct_locations(db_)};
}

}  // namespace devcard
