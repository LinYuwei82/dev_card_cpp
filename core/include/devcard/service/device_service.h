#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "devcard/audit.h"
#include "devcard/dao/device_dao.h"
#include "devcard/db.h"
#include "devcard/models.h"

namespace devcard {

struct DevicePage {
  std::vector<Device> items;
  std::int64_t total = 0;      // 当前筛选命中数（页脚与状态栏共用）
  std::int64_t total_all = 0;  // 无筛选总数（状态栏「筛选后 M / 共 N」用）
  int page = 0;
  int page_size = 20;
};

struct DeviceOptions {
  std::vector<std::string> names;
  std::vector<std::string> locations;
};

struct ImportReport {
  int imported = 0;       // 实际写入行数
  int skipped_empty = 0;  // 四字段全空、被跳过的行
  int duplicates = 0;     // 与现有记录三字段相同的行（仅提示，不阻断）
  int truncated = 0;      // 有字段被截断的行
};

// 台账业务规则：校验、手工新增三字段查重、分页边界、导入导出。
class DeviceService {
 public:
  explicit DeviceService(Db& db) : db_(db) {}

  DevicePage list(const DeviceQuery& query) const;
  Device get(std::int64_t id) const;
  Device create(Device device);
  Device create(Device device, const AuditContext& audit);
  Device update(std::int64_t id, Device device);
  Device update(std::int64_t id, Device device, const AuditContext& audit);
  void remove(std::int64_t id);
  void remove(std::int64_t id, const AuditContext& audit);
  DeviceOptions options() const;

  // 导入：表头必须与预期四列一致，否则整批拒绝（不允许错位）。
  // 去重口径按 D5：不去重、不跳过、不合并，重复只统计并提示。
  // 带 audit 时只记一条汇总，不逐行记。
  ImportReport import_rows(const std::vector<std::vector<std::string>>& rows);
  ImportReport import_rows(const std::vector<std::vector<std::string>>& rows,
                           const AuditContext& audit);

  // 导出：列头与导入一致，便于往返。
  std::vector<std::vector<std::string>> export_rows() const;

  static const std::vector<std::string>& expected_header();

 private:
  void validate(Device& device) const;

  Db& db_;
  DeviceDao dao_;
};

}  // namespace devcard
