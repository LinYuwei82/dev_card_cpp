#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "devcard/dao/device_dao.h"
#include "devcard/db.h"
#include "devcard/error.h"
#include "devcard/migrations.h"

namespace {

int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond     \
                << "\n";                                                   \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

std::filesystem::path temp_db() {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("devcard-test-" + std::to_string(std::rand()));
  std::filesystem::create_directories(dir);
  return dir / "devicedb.db";
}

}  // namespace

int main() {
  const std::filesystem::path file = temp_db();
  {
    devcard::Db db(file);
    CHECK(db.schema_version() == 0);
    devcard::migrate(db);
    CHECK(db.schema_version() == devcard::target_schema_version());
    devcard::migrate(db);  // 幂等
    CHECK(db.schema_version() == devcard::target_schema_version());

    devcard::DeviceDao dao;
    devcard::Device pump;
    pump.dev_name = "1号水泵";
    pump.location = "B1栋";
    pump.control_range = "1-3层";
    pump.phone = "13800000001";
    db.write([&](sqlite3* conn) { dao.insert(conn, pump); });
    CHECK(pump.id == 1);

    devcard::Device fan;
    fan.dev_name = "排风机";
    fan.location = "B1栋";
    fan.control_range = "屋顶";
    fan.phone = "13800000002";
    db.write([&](sqlite3* conn) { dao.insert(conn, fan); });

    devcard::DeviceQuery query;
    CHECK(dao.count(db, query) == 2);
    auto all = dao.list(db, query);
    CHECK(all.size() == 2);
    CHECK(all.front().dev_name == "1号水泵");

    query.keyword = "水泵";
    CHECK(dao.count(db, query) == 1);
    query.keyword.clear();
    query.name = "排风机";
    CHECK(dao.count(db, query) == 1);

    devcard::Device found;
    CHECK(dao.find(db, fan.id, found));
    CHECK(found.location == "B1栋");

    found.location = "A2栋";
    db.write([&](sqlite3* conn) { dao.update(conn, found); });
    devcard::Device updated;
    CHECK(dao.find(db, fan.id, updated));
    CHECK(updated.location == "A2栋");

    db.write([&](sqlite3* conn) { dao.erase(conn, pump.id); });
    CHECK(dao.count(db, devcard::DeviceQuery{}) == 1);

    query.name.clear();
    query.page_size = 1000;
    query.sort = "dev_name; DROP TABLE device";
    CHECK(dao.list(db, query).size() == 1);  // 非法排序被忽略

    bool rejected = false;
    try {
      devcard::Device bad;
      bad.dev_name = "";
      db.write([&](sqlite3* conn) { dao.insert(conn, bad); });
    } catch (const devcard::AppError&) {
      rejected = true;
    }
    CHECK(rejected);
  }

  {
    devcard::Db db(file);
    CHECK(db.schema_version() == devcard::target_schema_version());
  }

  std::filesystem::remove_all(file.parent_path());
  if (failures) {
    std::cerr << failures << " 项失败\n";
    return 1;
  }
  std::cout << "M0 core tests passed\n";
  return 0;
}
