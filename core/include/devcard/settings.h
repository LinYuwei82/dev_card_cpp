#pragma once

#include <string>
#include <string_view>

#include "devcard/db.h"

namespace devcard {

// app_setting 键值对。读接口返回空串表示不存在。
std::string get_setting(const Db& db, std::string_view key);
bool has_setting(const Db& db, std::string_view key);

// 写接口要求调用方已处于 Db::write() 事务内。
void set_setting(sqlite3* conn, std::string_view key, std::string_view value);

}  // namespace devcard
