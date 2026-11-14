#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace devcard {

// 旧 Qt 库：只有 tb_device / tb_user，没有 schema_migration。
struct LegacyInfo {
  bool is_legacy = false;
  std::int64_t devices = 0;
  std::int64_t users = 0;
};

LegacyInfo inspect_legacy(const std::filesystem::path& file);

struct LegacyImportReport {
  bool performed = false;
  std::string source;
  std::string source_sha256;
  std::string backup;
  std::string archived_as;
  std::int64_t devices_total = 0;
  std::int64_t devices_imported = 0;
  std::int64_t devices_skipped = 0;
  std::int64_t users_total = 0;
  std::int64_t users_imported = 0;
  std::int64_t users_skipped = 0;
  std::vector<std::string> warnings;
};

// 先备份并校验 SHA256，再只读导入；成功后旧库改名为 *.migrated-<时间戳>。
// 重复执行按 dev_id / user_name 幂等跳过。
LegacyImportReport import_legacy(const std::filesystem::path& source,
                                 const std::filesystem::path& data_dir);

// 启动时探测 <data_dir>/devicedb.db 与 <data_dir>/legacy/devicedb.db。
bool needs_legacy_import(const std::filesystem::path& data_dir);
LegacyImportReport auto_import_if_needed(const std::filesystem::path& data_dir);

}  // namespace devcard
