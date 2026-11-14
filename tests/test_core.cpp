#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "devcard/backup.h"
#include "devcard/dao/device_dao.h"
#include "devcard/db.h"
#include "devcard/error.h"
#include "devcard/legacy.h"
#include "devcard/migrations.h"
#include "devcard/password.h"
#include "devcard/service/device_service.h"
#include "devcard/service/user_service.h"
#include "devcard/settings.h"
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
  static std::uint64_t counter = 0;
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto dir = std::filesystem::temp_directory_path() /
                   ("devcard-test-" + std::to_string(stamp) + "-" +
                    std::to_string(counter++));
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

  {
    // Argon2id：编码串、正确/错误口令。
    const std::string hash = devcard::hash_password("Passw0rd!");
    CHECK(hash.rfind("$argon2id$", 0) == 0);
    CHECK(devcard::verify_password(hash, "Passw0rd!"));
    CHECK(!devcard::verify_password(hash, "passw0rd!"));
    CHECK(!devcard::verify_password("", "Passw0rd!"));
  }

  {
    const std::filesystem::path dir = temp_db().parent_path();
    const std::filesystem::path file = dir / "devicedb.db";
    devcard::Db db(file);
    devcard::migrate(db);

    const devcard::SeedResult seed = devcard::seed_default_users(db, dir);
    CHECK(seed.seeded);
    CHECK(seed.accounts.size() == 2);
    CHECK(std::filesystem::exists(seed.password_file));
    CHECK(!devcard::seed_default_users(db, dir).seeded);  // 幂等：已有账号不再重建

    devcard::UserService users(db);
    CHECK(users.count() == 2);

    const std::string root_password = seed.accounts[0].second;
    const devcard::User root = users.authenticate("root", root_password);
    CHECK(root.must_change_password);
    CHECK(users.local_user() == "root");

    bool bad_login = false;
    try {
      users.authenticate("root", "definitely-wrong");
    } catch (const devcard::AppError& error) {
      bad_login = std::string(error.code()) == "invalid_credentials";
    }
    CHECK(bad_login);

    for (int i = 0; i < 4; ++i) {
      try {
        users.authenticate("root", "definitely-wrong");
      } catch (const devcard::AppError&) {
      }
    }
    bool locked = false;
    try {
      users.authenticate("root", root_password);
    } catch (const devcard::AppError& error) {
      locked = std::string(error.code()) == "locked";
    }
    CHECK(locked);

    // 命令行恢复通道：重置后清锁、强制改密。
    users.reset_password("root", "ResetPassw0rd");
    const devcard::User reset = users.authenticate("root", "ResetPassw0rd");
    CHECK(reset.must_change_password);

    users.change_password("root", "ResetPassw0rd", "ChangedPassw0rd");
    CHECK(!users.get("root").must_change_password);
    bool old_rejected = false;
    try {
      users.authenticate("root", "ResetPassw0rd");
    } catch (const devcard::AppError&) {
      old_rejected = true;
    }
    CHECK(old_rejected);
    CHECK(users.authenticate("root", "ChangedPassw0rd").user_name == "root");

    bool weak = false;
    try {
      users.create("short", "123", "", "user");
    } catch (const devcard::AppError& error) {
      weak = std::string(error.code()) == "weak_password";
    }
    CHECK(weak);

    const devcard::User normal =
        users.create("tester", "TesterPassw0rd", "测试员", "user");
    CHECK(normal.role == "user");
    CHECK(normal.must_change_password);

    bool last_admin = false;
    try {
      users.remove("root", "tester");
    } catch (const devcard::AppError& error) {
      last_admin = std::string(error.code()) == "last_admin";
    }
    CHECK(last_admin);

    users.remove("tester", "root");
    CHECK(users.count() == 2);

    // 两个默认账号都改密后，一次性口令文件可删除。
    users.change_password("安业物业", seed.accounts[1].second, "AnyePassw0rd");
    CHECK(!users.any_must_change());
    devcard::remove_seed_password_file(dir);
    CHECK(!std::filesystem::exists(seed.password_file));

    std::filesystem::remove_all(dir);
  }

  {
    // 旧库：13 条设备 + 3 个用户（含默认账号与超长字段）。
    const std::filesystem::path dir = temp_db().parent_path();
    const std::filesystem::path legacy_dir = dir / "legacy";
    std::filesystem::create_directories(legacy_dir);
    const std::filesystem::path legacy_file = legacy_dir / "devicedb.db";
    {
      sqlite3* conn = nullptr;
      sqlite3_open(legacy_file.string().c_str(), &conn);
      devcard::exec_sql(
          conn,
          "CREATE TABLE tb_device(dev_id INTEGER PRIMARY KEY AUTOINCREMENT,"
          " dev_name VARCHAR(20), location VARCHAR(20), control_range VARCHAR(20),"
          " phone VARCHAR(20));"
          "CREATE TABLE tb_user(userName VARCHAR(20) PRIMARY KEY,"
          " userPwd VARCHAR(20));");
      devcard::exec_sql(conn, "BEGIN");
      for (int i = 1; i <= 12; ++i) {
        const std::string sql =
            "INSERT INTO tb_device(dev_name,location,control_range,phone)"
            " VALUES('设备" + std::to_string(i) + "','B" + std::to_string(i % 3) +
            "栋','1-" + std::to_string(i % 5) + "层','138000000" +
            (i < 10 ? "0" : "") + std::to_string(i) + "')";
        devcard::exec_sql(conn, sql);
      }
      devcard::exec_sql(conn,
                        "INSERT INTO tb_device(dev_name,location,control_range,phone)"
                        " VALUES('这是一个超过二十个字符的设备名称用来验证截断行为',"
                        "'A2栋','屋顶',NULL)");
      devcard::exec_sql(conn,
                        "INSERT INTO tb_user(userName,userPwd) VALUES"
                        "('root','123456'),('安业物业','123456'),('operator','pass123')");
      devcard::exec_sql(conn, "COMMIT");
      sqlite3_close(conn);
    }

    const devcard::LegacyInfo info = devcard::inspect_legacy(legacy_file);
    CHECK(info.is_legacy);
    CHECK(info.devices == 13);
    CHECK(info.users == 3);

    const devcard::LegacyImportReport report =
        devcard::import_legacy(legacy_file, dir);
    CHECK(report.performed);
    CHECK(report.devices_total == 13);
    CHECK(report.devices_imported == 13);
    CHECK(report.devices_skipped == 0);
    CHECK(report.users_imported == 3);
    CHECK(!report.backup.empty());
    CHECK(std::filesystem::exists(report.backup));
    CHECK(report.source_sha256.size() == 64);
    CHECK(!report.warnings.empty());  // 超长字段截断告警

    devcard::Db db(dir / "devicedb.db");
    devcard::migrate(db);
    devcard::DeviceDao devices;
    CHECK(devices.count(db, devcard::DeviceQuery{}) == 13);
    devcard::Device sample;
    CHECK(devices.find(db, 1, sample));
    CHECK(sample.dev_name == "设备1");
    CHECK(sample.location == "B1栋");
    CHECK(devices.find(db, 13, sample));
    CHECK(sample.phone.empty());  // NULL → ''
    CHECK(sample.dev_name.size() <= 60);  // 已按 20 个字符截断

    devcard::UserService users(db);
    CHECK(users.count() == 3);
    const devcard::User root = users.get("root");
    CHECK(root.role == "admin");
    CHECK(root.must_change_password);
    CHECK(root.password_hash.rfind("$argon2id$", 0) == 0);
    CHECK(root.password_hash.find("123456") == std::string::npos);
    CHECK(users.get("安业物业").must_change_password);
    CHECK(!users.get("operator").must_change_password);
    CHECK(devcard::get_setting(db, "legacy_imported_from") == report.source);
    CHECK(users.authenticate("root", "123456").must_change_password);

    // 重复导入：按 dev_id / user_name 幂等跳过，不追加。
    const devcard::LegacyImportReport again =
        devcard::import_legacy(legacy_file, dir);
    CHECK(again.performed);
    CHECK(again.devices_imported == 0);
    CHECK(again.devices_skipped == 13);
    CHECK(again.users_imported == 0);
    CHECK(again.users_skipped == 3);
    CHECK(devices.count(db, devcard::DeviceQuery{}) == 13);

    // 备份与恢复：删除一条后可从备份恢复。
    const devcard::BackupEntry backup =
        devcard::create_backup(db, dir, "test");
    CHECK(std::filesystem::exists(backup.path));
    CHECK(backup.bytes > 0);
    devcard::DeviceService service(db);
    service.remove(1);
    CHECK(devices.count(db, devcard::DeviceQuery{}) == 12);
    devcard::restore_backup(db, dir, backup.name);
    CHECK(devices.count(db, devcard::DeviceQuery{}) == 13);

    bool bad_name = false;
    try {
      devcard::restore_backup(db, dir, "../devicedb.db");
    } catch (const devcard::AppError& error) {
      bad_name = std::string(error.code()) == "bad_backup";
    }
    CHECK(bad_name);

    std::filesystem::remove_all(dir);
  }

  {
    // 导入模板：第一个工作表只有表头，且与导入校验共用同一份常量。
    const std::filesystem::path dir = temp_db().parent_path();
    const std::filesystem::path file = dir / "devicedb.db";
    devcard::Db db(file);
    devcard::migrate(db);
    devcard::DeviceService service(db);

    devcard::xlsx::Sheet data;
    data.name = "设备台账";
    data.rows.push_back(devcard::DeviceService::expected_header());
    devcard::xlsx::Sheet help;
    help.name = "填写说明";
    help.rows = {{"设备名称", "必填"}, {"示例（假号码）", "13800000001"}};

    const std::string bytes = devcard::xlsx::write_workbook({data, help});
    CHECK(!bytes.empty());
    const auto rows = devcard::xlsx::read(bytes);
    CHECK(rows.size() == 1);
    CHECK(rows[0] == devcard::DeviceService::expected_header());

    // 模板原样上传 = 0 行导入，不会带入假数据。
    const devcard::ImportReport report = service.import_rows(rows);
    CHECK(report.imported == 0);
    CHECK(report.skipped_empty == 0);
    CHECK(report.duplicates == 0);
    CHECK(service.export_rows().size() == 1);  // 只剩表头

    std::filesystem::remove_all(dir);
  }

  std::filesystem::remove_all(file.parent_path());
  if (failures) {
    std::cerr << failures << " 项失败\n";
    return 1;
  }
  std::cout << "core tests passed\n";
  return 0;
}
