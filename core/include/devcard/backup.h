#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "devcard/db.h"

namespace devcard {

struct BackupEntry {
  std::string name;
  std::string path;
  std::int64_t bytes = 0;
  std::string created_at;
};

std::filesystem::path backups_dir(const std::filesystem::path& data_dir);

// VACUUM INTO 生成一致性快照，返回新备份信息。
BackupEntry create_backup(Db& db, const std::filesystem::path& data_dir,
                          std::string_view label);

std::vector<BackupEntry> list_backups(const std::filesystem::path& data_dir);

// 恢复：先为当前库再做一份 pre-restore 备份，再在一个事务里用备份内容替换
// device / app_user / app_setting。
void restore_backup(Db& db, const std::filesystem::path& data_dir,
                    std::string_view name);

}  // namespace devcard
