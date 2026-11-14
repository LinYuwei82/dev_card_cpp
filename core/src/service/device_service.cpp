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

// 按 UTF-8 码点截断，不切断多字节字符。
std::string truncate_utf8(const std::string& value, std::size_t max_chars) {
  std::size_t chars = 0;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if ((static_cast<unsigned char>(value[i]) & 0xC0) == 0x80) continue;
    if (chars == max_chars) return value.substr(0, i);
    ++chars;
  }
  return value;
}

std::string join(const std::vector<std::string>& parts, const char* sep) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i) out += sep;
    out += parts[i].empty() ? "（空）" : parts[i];
  }
  return out;
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

const std::vector<std::string>& DeviceService::expected_header() {
  static const std::vector<std::string> header = {"设备名称", "位置", "控制范围",
                                                  "维护电话"};
  return header;
}

ImportReport DeviceService::import_rows(
    const std::vector<std::vector<std::string>>& rows) {
  if (rows.empty())
    throw AppError("bad_header", "导入文件是空的：第 1 行必须是表头");

  const auto& expected = expected_header();
  std::vector<std::string> actual;
  actual.reserve(expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    actual.push_back(trim(i < rows[0].size() ? rows[0][i] : std::string()));
  }
  if (actual != expected) {
    throw AppError("bad_header",
                   "导入文件的表头与要求不一致：第 1 行应为「设备名称 / 位置 / "
                   "控制范围 / 维护电话」，实际为「" +
                       join(actual, " / ") +
                       "」。请调整列顺序后重试，导入不会自动错位对齐。");
  }

  static constexpr std::size_t kLimits[4] = {10, 10, 10, 11};
  ImportReport report;
  db_.write([&](sqlite3* conn) {
    for (std::size_t r = 1; r < rows.size(); ++r) {
      std::vector<std::string> cells(4);
      for (std::size_t c = 0; c < 4; ++c) {
        cells[c] = trim(c < rows[r].size() ? rows[r][c] : std::string());
      }
      if (cells[0].empty() && cells[1].empty() && cells[2].empty() &&
          cells[3].empty()) {
        ++report.skipped_empty;
        continue;
      }
      bool truncated = false;
      for (std::size_t c = 0; c < 4; ++c) {
        const std::string cut = truncate_utf8(cells[c], kLimits[c]);
        if (cut != cells[c]) truncated = true;
        cells[c] = cut;
      }
      if (truncated) ++report.truncated;

      Device device;
      device.dev_name = cells[0];
      device.location = cells[1];
      device.control_range = cells[2];
      device.phone = cells[3];
      if (dao_.exists_triplet(conn, device.dev_name, device.location,
                              device.control_range)) {
        ++report.duplicates;
      }
      dao_.insert(conn, device);
      ++report.imported;
    }
  });
  return report;
}

std::vector<std::vector<std::string>> DeviceService::export_rows() const {
  std::vector<std::vector<std::string>> rows;
  rows.push_back(expected_header());

  DeviceQuery query;
  query.page_size = 1000;
  query.sort = "id";
  const std::int64_t total = dao_.count(db_, query);
  for (std::int64_t page = 0; page * query.page_size < total; ++page) {
    query.page = static_cast<int>(page);
    for (const Device& device : dao_.list(db_, query)) {
      rows.push_back({device.dev_name, device.location, device.control_range,
                      device.phone});
    }
  }
  return rows;
}

}  // namespace devcard
