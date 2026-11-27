#pragma once

#include <filesystem>
#include <string>

namespace devcard {

class Conn;
class Db;

// 备份/恢复的后端抽象入口：只定义「导出副本 / 校验备份 / 整表恢复」三件事。
// SQLite 实现（VACUUM INTO / ATTACH / 整表拷贝）见 core/src/backend/sqlite_backup_dao.cpp；
// PostgreSQL 实现（逻辑导出/导入）在阶段 2 按同一接口新增。
class BackupDao {
 public:
  virtual ~BackupDao() = default;

  // 回收空间并落一份完整副本（自带 write_raw，不进入外层事务）。
  virtual void vacuum_into(Db& db, const std::string& target_utf8) const = 0;

  // 备份文件必须是本程序的库：必需表齐全且完整性检查通过。
  virtual void verify_backup_file(const std::filesystem::path& file) const = 0;

  // 用备份文件整表替换 device / app_user / app_setting。
  // 自带事务与挂载/卸载顺序管理（ATTACH/DETACH 不能进事务）。
  virtual void restore_from(Db& db, const std::string& source_utf8) const = 0;
};

}  // namespace devcard
