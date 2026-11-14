#pragma once

#include <string_view>

namespace devcard::dialect {

// SQLite 方言点集中在这里。换后端时只改本文件 + DAO 中少量调用，
// 不引入接口化 Dialect、不做 SQL 生成器（D8）。

inline constexpr std::string_view kAutoPk = "INTEGER PRIMARY KEY AUTOINCREMENT";
inline constexpr std::string_view kBoolType = "INTEGER";
inline constexpr std::string_view kNow = "datetime('now','localtime')";

// 大小写不敏感比较：SQLite 用 NOCASE，其他后端需换 lower()/citext。
inline constexpr std::string_view kCaseInsensitive = "COLLATE NOCASE";

}  // namespace devcard::dialect
