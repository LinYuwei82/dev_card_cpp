#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "devcard/dao/device_dao.h"
#include "devcard/db.h"
#include "devcard/error.h"
#include "devcard/migrations.h"
#include "devcard/service/device_service.h"
#include "devcard/xlsx.h"

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

  {
    const std::filesystem::path service_file = temp_db();
    {
      devcard::Db db(service_file);
      devcard::migrate(db);
      devcard::DeviceService service(db);

      auto make = [](std::string name, std::string location,
                     std::string range, std::string phone) {
        devcard::Device device;
        device.dev_name = std::move(name);
        device.location = std::move(location);
        device.control_range = std::move(range);
        device.phone = std::move(phone);
        return device;
      };

      const devcard::Device first =
          service.create(make(" 1号水泵 ", "B1栋", "1-3层", "13800000001"));
      CHECK(first.dev_name == "1号水泵");
      CHECK(first.id > 0);
      service.create(make("2号水泵", "B1栋", "1-3层", "13800000002"));
      service.create(make("排风机", "B1栋", "屋顶", "13800000003"));
      service.create(make("空调机组", "A2栋", "2层", "13800000004"));
      service.create(make("照明配电箱", "A2栋", "1层", "13800000005"));

      bool duplicate = false;
      try {
        service.create(make("1号水泵", "B1栋", "1-3层", "13800000009"));
      } catch (const devcard::AppError& error) {
        duplicate = std::string(error.code()) == "duplicate";
      }
      CHECK(duplicate);

      bool too_long = false;
      try {
        service.create(make(std::string(21, 'a'), "B1栋", "1层", "138"));
      } catch (const devcard::AppError& error) {
        too_long = std::string(error.code()) == "invalid_device";
      }
      CHECK(too_long);

      bool empty_field = false;
      try {
        service.create(make("设备", "   ", "1层", "138"));
      } catch (const devcard::AppError& error) {
        empty_field = std::string(error.code()) == "invalid_device";
      }
      CHECK(empty_field);

      const devcard::DeviceOptions options = service.options();
      CHECK(options.locations.size() == 2);
      CHECK(options.locations.front() == "A2栋");
      CHECK(options.locations.back() == "B1栋");
      CHECK(options.names.size() == 5);

      devcard::DeviceQuery query;
      query.page_size = 2;
      const devcard::DevicePage page0 = service.list(query);
      CHECK(page0.total == 5);
      CHECK(page0.items.size() == 2);
      query.page = 2;
      const devcard::DevicePage page2 = service.list(query);
      CHECK(page2.items.size() == 1);
      CHECK(page2.page == 2);

      devcard::DeviceQuery by_location;
      by_location.location = "A2栋";
      CHECK(service.list(by_location).total == 2);

      devcard::DeviceQuery by_keyword;
      by_keyword.keyword = "水泵";
      by_keyword.field = "dev_name";
      CHECK(service.list(by_keyword).total == 2);

      devcard::Device edited = first;
      edited.phone = "13900000000";
      const devcard::Device saved = service.update(first.id, edited);
      CHECK(saved.phone == "13900000000");
      CHECK(service.get(first.id).phone == "13900000000");

      bool missing_update = false;
      try {
        service.update(99999, make("不存在", "B1栋", "1层", "138"));
      } catch (const devcard::AppError& error) {
        missing_update = std::string(error.code()) == "not_found";
      }
      CHECK(missing_update);

      service.remove(first.id);
      CHECK(service.list(devcard::DeviceQuery{}).total == 4);
      bool missing_remove = false;
      try {
        service.remove(first.id);
      } catch (const devcard::AppError& error) {
        missing_remove = std::string(error.code()) == "not_found";
      }
      CHECK(missing_remove);
    }
    std::filesystem::remove_all(service_file.parent_path());
  }

  {
    const std::filesystem::path import_file = temp_db();
    {
      devcard::Db db(import_file);
      devcard::migrate(db);
      devcard::DeviceService service(db);

      const std::vector<std::vector<std::string>> rows = {
          {"设备名称", "位置", "控制范围", "维护电话"},
          {"1号泵", "B1栋", "1-3层", "13800000001"},
          {"2号泵", "B1栋", "1-3层", "13800000002"},
          {"风机", "A2栋", "屋顶", "13800000003"},
          {"", "", "", ""},
          {"一二三四五六七八九十十一", "A2栋", "1层", "13800000004"},
      };

      const devcard::ImportReport first = service.import_rows(rows);
      CHECK(first.imported == 4);
      CHECK(first.skipped_empty == 1);
      CHECK(first.truncated == 1);
      CHECK(first.duplicates == 0);

      const devcard::ImportReport second = service.import_rows(rows);
      CHECK(second.imported == 4);
      CHECK(second.duplicates == 4);
      CHECK(second.skipped_empty == 1);

      devcard::DeviceQuery query;
      query.page_size = 1000;
      CHECK(service.list(query).total == 8);

      // 表头不对整批拒绝，且不允许错位对齐。
      bool bad_header = false;
      try {
        service.import_rows({{"位置", "设备名称", "控制范围", "维护电话"},
                             {"B1栋", "3号泵", "1层", "138"}});
      } catch (const devcard::AppError& error) {
        bad_header = std::string(error.code()) == "bad_header";
      }
      CHECK(bad_header);
      CHECK(service.list(query).total == 8);

      const auto exported = service.export_rows();
      CHECK(exported.size() == 9);
      CHECK(exported.front() == devcard::DeviceService::expected_header());
      const std::string bytes = devcard::xlsx::write(exported);
      CHECK(!bytes.empty());
      const auto roundtrip = devcard::xlsx::read(bytes);
      CHECK(roundtrip.size() == exported.size());
      CHECK(roundtrip.front() == exported.front());
      CHECK(roundtrip[1] == exported[1]);
      CHECK(roundtrip.back() == exported.back());
    }
    std::filesystem::remove_all(import_file.parent_path());
  }

  {
    const std::filesystem::path legacy_file = temp_db();
    {
      devcard::Db db(legacy_file);
      db.write_raw([](sqlite3* conn) {
        devcard::exec_sql(
            conn,
            "CREATE TABLE tb_device(dev_id INTEGER PRIMARY KEY, dev_name TEXT)");
      });
      bool rejected = false;
      try {
        devcard::migrate(db);
      } catch (const devcard::AppError& error) {
        rejected = std::string(error.code()) == "legacy_db";
      }
      CHECK(rejected);
    }
    std::filesystem::remove_all(legacy_file.parent_path());
  }

  std::filesystem::remove_all(file.parent_path());
  if (failures) {
    std::cerr << failures << " 项失败\n";
    return 1;
  }
  std::cout << "core tests passed\n";
  return 0;
}
