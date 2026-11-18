#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "devcard/audit.h"
#include "devcard/dao/device_dao.h"
#include "devcard/db.h"
#include "devcard/error.h"
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
  int skipped_invalid = 0;  // 必填字段缺失、被跳过的行
  int duplicates = 0;     // 与现有记录三字段相同的行（仅提示，不阻断）
  int truncated = 0;      // 有字段被截断的行
  std::vector<int> invalid_rows;  // 被跳过的 Excel 行号（最多保留前 20 个）
};

// 合并重复的结果：删除的行数与合并后剩余总数。
struct DedupeReport {
  std::int64_t removed = 0;
  std::int64_t remaining = 0;
};

// 乐观锁冲突时的现场：库里最新一行 + 与提交值不同的字段名。
struct ConflictSnapshot {
  Device current;
  std::vector<std::string> fields;
};

// 409 冲突：调用方带着过期 version 提交，库里已经被别人改过。
// 响应体将带上 current/version/fields，足以让前端提示「刷新后重填」；
// 是否再给管理员一个强制覆盖入口属于 P3 未拍板项，这里先把数据备齐但不提供接口。
class ConflictError : public AppError {
 public:
  ConflictError(std::string message, ConflictSnapshot snapshot)
      : AppError("conflict", std::move(message)), snapshot_(std::move(snapshot)) {}

  const ConflictSnapshot& snapshot() const { return snapshot_; }

 private:
  ConflictSnapshot snapshot_;
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
  // P3 管理员强制覆盖：无视版本冲突，但版本仍从库里当前值 +1（不倒退），
  // updated_by 记实际操作人；审计 action=force_overwrite，before 快照保留
  // 被覆盖版本的 updated_by/updated_at/version，普通 update 无法伪装成它。
  // 调用方必须保证 audit.actor 是管理员身份。
  Device force_update(std::int64_t id, Device device, const AuditContext& audit);
  void remove(std::int64_t id, std::int64_t version);
  void remove(std::int64_t id, std::int64_t version, const AuditContext& audit);
  // 合并三字段重复：每组保留 id 最小的一行，电话为空时从重复行补齐。
  // 与导入策略（D5：照插、只统计）不冲突——这是用户显式触发的一次性清理。
  DedupeReport dedupe(const AuditContext& audit);
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
