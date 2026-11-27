#pragma once

#include "devcard/dao/backup_dao.h"

namespace devcard {

// BackupDao 的 SQLite 实现：VACUUM INTO / ATTACH / PRAGMA 只允许出现在本文件。
class SqliteBackupDao final : public BackupDao {
 public:
  void vacuum_into(Db& db, const std::string& target_utf8) const override;
  void verify_backup_file(const std::filesystem::path& file) const override;
  void restore_from(Db& db, const std::string& source_utf8) const override;
};

}  // namespace devcard
