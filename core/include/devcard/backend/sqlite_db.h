#pragma once

#include <filesystem>
#include <memory>

#include "devcard/db.h"

namespace devcard {

// SQLite 后端工厂：单写连接（WAL + busy timeout）+ 小读池。
// 实现见 core/src/backend/sqlite_db.cpp。
// 阶段 2 会并列提供 open_postgres_db（DSN 入参），装配层按配置二选一。
std::unique_ptr<Db> open_sqlite_db(std::filesystem::path file,
                                   int read_connections = 2);

}  // namespace devcard
