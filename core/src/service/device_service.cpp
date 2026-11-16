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

std::string device_json_text(const Device& device) {
  return "{\"id\":" + std::to_string(device.id) + ",\"dev_name\":\"" +
         json_escape(device.dev_name) + "\",\"location\":\"" +
         json_escape(device.location) + "\",\"control_range\":\"" +
         json_escape(device.control_range) + "\",\"phone\":\"" +
         json_escape(device.phone) + "\",\"updated_at\":\"" +
         json_escape(device.updated_at) + "\",\"version\":" +
         std::to_string(device.version) + ",\"updated_by\":\"" +
         json_escape(device.updated_by) + "\"}";
}

AuditEntry device_audit(const AuditContext& context, const char* action,
                        std::int64_t id, const std::string& summary) {
  AuditEntry entry;
  entry.actor = context.actor;
  entry.actor_ip = context.actor_ip;
  entry.action = action;
  entry.entity = "device";
  entry.entity_id = id > 0 ? std::to_string(id) : std::string();
  entry.summary = summary;
  return entry;
}

// 冲突时给调用方看的字段差异：只报「你提交的」与「库里最新」不同的列，
// 前端可以据此只高亮需要重填的字段。
std::vector<std::string> changed_fields(const Device& input,
                                        const Device& current) {
  std::vector<std::string> fields;
  if (input.dev_name != current.dev_name) fields.push_back("dev_name");
  if (input.location != current.location) fields.push_back("location");
  if (input.control_range != current.control_range) {
    fields.push_back("control_range");
  }
  if (input.phone != current.phone) fields.push_back("phone");
  return fields;
}

std::string conflict_message(const Device& current) {
  const std::string who = current.updated_by.empty() ? "他人" : current.updated_by;
  return "这条记录已被 " + who + " 于 " + current.updated_at +
         " 修改，你的修改未保存；请刷新后重填。";
}

void require_version(std::int64_t version) {
  if (version <= 0) {
    throw AppError("bad_version",
                   "缺少 version（记录版本号）。请刷新列表后重新编辑，"
                   "不要用过期页面覆盖别人的修改。");
  }
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

  // 状态栏需要「筛选后 M / 共 N」的 N。无筛选时两个数相同，不再多查一次。
  const bool filtered = !normalized.keyword.empty() || !normalized.field.empty() ||
                        !normalized.name.empty() || !normalized.location.empty();
  result.total_all =
      filtered ? dao_.count(db_, DeviceQuery{}) : result.total;
  return result;
}

Device DeviceService::get(std::int64_t id) const {
  Device device;
  if (!dao_.find(db_, id, device))
    throw AppError("not_found", "设备不存在");
  return device;
}

Device DeviceService::create(Device device) {
  return create(std::move(device), AuditContext{});
}

Device DeviceService::create(Device device, const AuditContext& audit) {
  validate(device);
  db_.write([&](sqlite3* conn) {
    if (dao_.exists_triplet(conn, device.dev_name, device.location,
                            device.control_range)) {
      throw AppError("duplicate", "你要添加的设备己经存在，请重新输入");
    }
    dao_.insert(conn, device);
    if (audit.enabled()) {
      AuditEntry entry = device_audit(audit, "create", device.id, "新增设备");
      entry.after_json = device_json_text(device);
      record_audit(conn, entry);
    }
  });
  return device;
}

Device DeviceService::update(std::int64_t id, Device device) {
  return update(id, std::move(device), AuditContext{});
}

// 事务边界：读旧值 → 条件 UPDATE（version 匹配才写、写完 +1）→ 写审计，
// 三步在同一个 BEGIN IMMEDIATE 里；任何一步失败整体 ROLLBACK，
// 不会出现「数据已改但版本没加」或「数据改了但审计没写」的半成品。
Device DeviceService::update(std::int64_t id, Device device,
                             const AuditContext& audit) {
  validate(device);
  require_version(device.version);
  device.id = id;
  Device saved;
  db_.write([&](sqlite3* conn) {
    // 先在同一事务里取旧值：审计的 before 与「行是否存在」都基于同一次快照。
    Device before;
    if (!dao_.find(conn, id, before)) throw AppError("not_found", "设备不存在");
    if (!dao_.update(conn, device, audit.actor)) {
      // 版本不匹配时再取一次当前值：区分「刚被删除」与「被别人改过」。
      Device current;
      if (!dao_.find(conn, id, current)) {
        throw AppError("not_found", "设备不存在");
      }
      throw ConflictError(conflict_message(current),
                          ConflictSnapshot{current,
                                           changed_fields(device, current)});
    }
    if (!dao_.find(conn, id, saved)) throw AppError("not_found", "设备不存在");
    if (audit.enabled()) {
      AuditEntry entry = device_audit(audit, "update", id, "修改设备");
      entry.before_json = device_json_text(before);
      entry.after_json = device_json_text(saved);
      record_audit(conn, entry);
    }
  });
  return saved;
}

Device DeviceService::force_update(std::int64_t id, Device device,
                                   const AuditContext& audit) {
  validate(device);
  if (!audit.enabled()) {
    throw AppError("forbidden", "强制覆盖必须带明确的操作人");
  }
  Device saved;
  db_.write([&](sqlite3* conn) {
    Device before;
    if (!dao_.find(conn, id, before)) throw AppError("not_found", "设备不存在");
    // 覆盖不是回退：以库里当前版本为基准再 +1，旧页面上的 version 直接忽略。
    device.id = id;
    device.version = before.version;
    if (!dao_.update(conn, device, audit.actor)) {
      // 同一写事务内刚读过，正常不会失败；真失败说明行被删除，按 404 处理。
      throw AppError("not_found", "设备不存在");
    }
    if (!dao_.find(conn, id, saved)) throw AppError("not_found", "设备不存在");
    // 与普通 update 明确区分：action 不同，summary 写清被抹掉的是谁的哪一版，
    // before_json 里保留当时的 updated_by/updated_at/version。
    const std::string loser =
        before.updated_by.empty() ? std::string("未知用户") : before.updated_by;
    AuditEntry entry = device_audit(
        audit, "force_overwrite", id,
        "强制覆盖：抹掉 " + loser + " 于 " + before.updated_at + " 保存的 v" +
            std::to_string(before.version) + "，生成 v" +
            std::to_string(saved.version));
    entry.before_json = device_json_text(before);
    entry.after_json = device_json_text(saved);
    record_audit(conn, entry);
  });
  return saved;
}

void DeviceService::remove(std::int64_t id, std::int64_t version) {  remove(id, version, AuditContext{});
}

void DeviceService::remove(std::int64_t id, std::int64_t version,
                           const AuditContext& audit) {
  require_version(version);
  db_.write([&](sqlite3* conn) {
    Device before;
    if (!dao_.find(conn, id, before)) throw AppError("not_found", "设备不存在");
    // 删除同样带版本：别人刚改过时拒绝，避免旧页面把新数据删掉。
    if (!dao_.erase(conn, id, version)) {
      throw ConflictError(conflict_message(before),
                          ConflictSnapshot{before, {}});
    }
    if (audit.enabled()) {
      AuditEntry entry = device_audit(audit, "delete", id, "删除设备");
      entry.before_json = device_json_text(before);
      record_audit(conn, entry);
    }
  });
}

DeviceOptions DeviceService::options() const {
  return {dao_.distinct_names(db_), dao_.distinct_locations(db_)};
}

DedupeReport DeviceService::dedupe(const AuditContext& audit) {
  DedupeReport report;
  db_.write([&](sqlite3* conn) {
    report.removed = dao_.dedupe(conn, audit.actor);
    if (audit.enabled()) {
      // 删除是整批动作，只记一条汇总；逐行记会让审计里塞满同类条目。
      AuditEntry entry = device_audit(
          audit, "dedupe", 0,
          "合并重复设备：删除 " + std::to_string(report.removed) + " 条");
      record_audit(conn, entry);
    }
  });
  report.remaining = dao_.count(db_, DeviceQuery{});
  return report;
}

const std::vector<std::string>& DeviceService::expected_header() {
  static const std::vector<std::string> header = {"设备名称", "位置", "控制范围",
                                                  "维护电话"};
  return header;
}

ImportReport DeviceService::import_rows(
    const std::vector<std::vector<std::string>>& rows) {
  return import_rows(rows, AuditContext{});
}

ImportReport DeviceService::import_rows(
    const std::vector<std::vector<std::string>>& rows,
    const AuditContext& audit) {
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
    if (audit.enabled()) {
      // 导入只记一条汇总；逐行记会让一次 5000 行导入写出 5000 条审计。
      AuditEntry entry = device_audit(
          audit, "import", 0,
          "导入 " + std::to_string(report.imported) + " 行（跳过空行 " +
              std::to_string(report.skipped_empty) + "，重复 " +
              std::to_string(report.duplicates) + "，截断 " +
              std::to_string(report.truncated) + "）");
      record_audit(conn, entry);
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
