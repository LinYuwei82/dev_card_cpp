#pragma once

#include <string>

#include <sqlite3.h>

namespace devcard {

// 备份/恢复的 SQL 出口：VACUUM、ATTACH 与整表拷贝都只在本类实现中。
class BackupDao {
 public:
  // 回收空间并落一份完整副本（不进入事务）。
  void vacuum_into(sqlite3* conn, const std::string& target_utf8) const;

  // 备份文件必须是本程序的库：必需表齐全且 integrity_check 为 ok。
  void verify_database(sqlite3* conn) const;

  // 用备份文件整表替换 device / app_user / app_setting。
  // 调用方负责开写事务之外的 ATTACH/DETACH 顺序；本方法内部自带事务。
  void restore_from(sqlite3* conn, const std::string& source_utf8) const;
};

}  // namespace devcard
