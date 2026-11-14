#pragma once

#include <string_view>
#include <vector>

#include "devcard/db.h"

namespace devcard {

struct Migration {
  int version;
  const char* name;
  const char* sql;
};

const std::vector<Migration>& all_migrations();
int target_schema_version();

// 校验已应用迁移的 checksum，并按版本顺序执行未应用项。
void migrate(Db& db);

}  // namespace devcard
