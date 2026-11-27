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

// 心跳 upsert：SQLite 用 INSERT OR REPLACE；PG（阶段 2）用
// INSERT ... ON CONFLICT(id) DO UPDATE SET。唯一消费点 instance_dao.cpp。
inline constexpr std::string_view kHeartbeatUpsert = "INSERT OR REPLACE INTO";

// 大小写不敏感匹配：SQLite 的 LIKE 对 ASCII 天然不敏感；PG（阶段 2）需 ILIKE。
// 唯一消费点 device_dao.cpp 的筛选子句。
inline constexpr std::string_view kLikeOp = "LIKE";

}  // namespace devcard::dialect
