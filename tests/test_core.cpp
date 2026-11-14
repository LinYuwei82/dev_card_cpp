#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "devcard/backup.h"
#include "devcard/dao/device_dao.h"
#include "devcard/db.h"
#include "devcard/error.h"
#include "devcard/legacy.h"
#include "devcard/migrations.h"
#include "devcard/password.h"
#include "devcard/paths.h"
#include "devcard/service/device_service.h"
#include "devcard/service/user_service.h"
#include "devcard/settings.h"
#include "devcard/sha256.h"
#include "devcard/update_package.h"
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


// 删目录之前，连接必须已经关掉。
// 判据用 WAL 残留：库开了 journal_mode=WAL，连接还活着时 -wal/-shm 一定在，
// 正常关闭（最后一个连接）会 checkpoint 并删掉它们。POSIX 允许删打开中的文件，
// 所以「目录删得掉」不能当判据，-wal 残留才可以——这一条在 macOS 上也会红。
void check_db_closed(const std::filesystem::path& dir) {
  // 侧车文件直接在 path 上追加 ASCII 后缀，不经过 .string()：
  // Windows 上窄字符往返走 ACP，中文目录会被改写。
  std::filesystem::path db = dir / "devicedb.db";
  std::filesystem::path wal = db;
  wal += "-wal";
  std::filesystem::path shm = db;
  shm += "-shm";
  CHECK(!std::filesystem::exists(wal));
  CHECK(!std::filesystem::exists(shm));
}

// UTF-8 码点数：续字节（10xxxxxx）不计。
int utf8_codepoints(const std::string& text) {
  int count = 0;
  for (const unsigned char byte : text) {
    if ((byte & 0xC0) != 0x80) ++count;
  }
  return count;
}
}  // namespace

// 自更新阶段一：白名单、文件名清洗、SHA256 + 字节数双校验、失败删残件。
void run_update_package_case(const std::filesystem::path& dir) {
  using namespace devcard;
  const std::string good_sha = std::string(64, 'a');

  CHECK(is_trusted_update_url(
      "https://github.com/LinYuwei82/dev_card_cpp/releases/download/v1/DevCard.dmg"));
  CHECK(is_trusted_update_url("https://objects.githubusercontent.com/pkg"));
  CHECK(!is_trusted_update_url("http://github.com/x"));
  CHECK(!is_trusted_update_url("https://evil.example/x"));
  CHECK(!is_trusted_update_url("https://github.com.evil.example/x"));
  CHECK(!is_trusted_update_url("https://user@github.com/x"));
  CHECK(!is_trusted_update_url("https://github.com:8443/x"));
  CHECK(!is_trusted_update_url("https://github.com"));

  CHECK(sanitize_update_file_name("../../a:b?.dmg") == "a_b_.dmg");
  CHECK(sanitize_update_file_name("C:\\tmp\\pkg.zip") == "pkg.zip");
  CHECK(sanitize_update_file_name("..") == "update.bin");
  CHECK(is_valid_sha256(good_sha));
  CHECK(!is_valid_sha256("abc"));

  const std::filesystem::path fixture = dir / "fixture.bin";
  {
    std::ofstream out(fixture, std::ios::binary);
    out << "devcard-update-fixture";
  }
  const std::string hash = sha256_file(fixture);
  const std::uint64_t size =
      static_cast<std::uint64_t>(std::filesystem::file_size(fixture));
  CHECK(verify_update_file(fixture, size, hash).ok);
  CHECK(!verify_update_file(fixture, size + 1, hash).ok);
  const auto wrong_hash = verify_update_file(fixture, size, good_sha);
  CHECK(!wrong_hash.ok);
  CHECK(wrong_hash.error.find("SHA256") != std::string::npos);

  UpdatePackageSpec spec;
  spec.version = "1.2.3";
  spec.file_name = "../DevCard-1.2.3.zip";
  spec.url =
      "https://github.com/example/releases/download/v1.2.3/DevCard-1.2.3.zip";
  spec.sha256 = hash;
  spec.size = size;
  CHECK(validate_update_spec(spec).empty());
  {
    UpdatePackageSpec insecure = spec;
    insecure.url = "http://github.com/example/pkg.zip";
    CHECK(!validate_update_spec(insecure).empty());
  }

  const std::filesystem::path updates = dir / "updates";
  auto copy_fetcher = [&fixture](const std::string&,
                                 const std::filesystem::path& target,
                                 std::string*) {
    std::filesystem::copy_file(fixture, target,
                               std::filesystem::copy_options::overwrite_existing);
    return true;
  };
  const UpdateDownloadOutcome ok =
      download_update_package(updates, spec, copy_fetcher);
  CHECK(ok.ok);
  CHECK(ok.path == devcard::path_to_utf8(updates / "DevCard-1.2.3.zip"));
  CHECK(std::filesystem::exists(ok.path));
  CHECK(!std::filesystem::exists(updates / "DevCard-1.2.3.zip.part"));
  CHECK(ok.sha256 == hash);
  CHECK(ok.bytes == size);

  const std::filesystem::path failed_dir = dir / "updates-failed";
  auto offline_fetcher = [](const std::string&,
                            const std::filesystem::path& target,
                            std::string* error) {
    std::ofstream out(target, std::ios::binary);
    out << "partial";
    if (error) *error = "模拟断网";
    return false;
  };
  const UpdateDownloadOutcome offline =
      download_update_package(failed_dir, spec, offline_fetcher);
  CHECK(!offline.ok);
  CHECK(offline.error.find("模拟断网") != std::string::npos);
  CHECK(!std::filesystem::exists(failed_dir / "DevCard-1.2.3.zip.part"));
  CHECK(!std::filesystem::exists(failed_dir / "DevCard-1.2.3.zip"));

  auto corrupt_fetcher = [](const std::string&,
                            const std::filesystem::path& target,
                            std::string*) {
    std::ofstream out(target, std::ios::binary);
    out << "corrupted";
    return true;
  };
  const UpdateDownloadOutcome corrupt =
      download_update_package(failed_dir, spec, corrupt_fetcher);
  CHECK(!corrupt.ok);
  CHECK(corrupt.error.find("校验失败") != std::string::npos);
  CHECK(!std::filesystem::exists(failed_dir / "DevCard-1.2.3.zip.part"));
  CHECK(!std::filesystem::exists(failed_dir / "DevCard-1.2.3.zip"));
}

// 场景函数：Db 是局部对象，函数返回即析构，调用方随后删目录才不会撞上打开的库。
void run_user_service_case(const std::filesystem::path& dir) {
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

  // 首次登录 / 重置后的强制改密不要求旧密码；但只有服务端标记
  // must_change_password 时才放行，普通状态仍必须走 change_password。
  users.reset_password("root", "ResetPassw0rd2");
  users.complete_forced_change("root", "ForcedPassw0rd");
  CHECK(!users.get("root").must_change_password);
  CHECK(users.authenticate("root", "ForcedPassw0rd").user_name == "root");
  bool forced_guard = false;
  try {
    users.complete_forced_change("root", "SneakyPassw0rd");
  } catch (const devcard::AppError& error) {
    forced_guard = std::string(error.code()) == "old_password_required";
  }
  CHECK(forced_guard);

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

  // 两个默认账号都改密后，一次性密码文件可删除。
  users.change_password("安业物业", seed.accounts[1].second, "AnyePassw0rd");
  CHECK(!users.any_must_change());
  devcard::remove_seed_password_file(dir);
  CHECK(!std::filesystem::exists(seed.password_file));

}

// 中文路径判据：只看磁盘上实际的目录项码元，不看 path / 界面怎么显示。
// Windows 上 path 的窄字符构造按 ACP（简中即 GBK）解释字节，源码里的 UTF-8
// 会落成 13 个乱码码元（9352 6FC6 E750 7490 FE40 5F7F 9359 FF44 62A4 002E ...），
// 而正确值是 10 个码元。这条用例在 Windows 上必须红过一次，否则等于没测。
void run_path_encoding_case(const std::filesystem::path& dir) {
  const std::string expected = "初始账号密码.txt";
  CHECK(utf8_codepoints(expected) == 10);

  // 1) 内存里来回一趟不能变形（Windows 上这里就会暴露 ACP 解释）。
  CHECK(devcard::path_to_utf8(devcard::path_from_utf8(expected)) == expected);
  // ACP（GBK）表达不了 emoji：这条能区分「按 UTF-8 转」和「按 ACP 转」。
  const std::string emoji = "样张-📇.pdf";
  CHECK(devcard::path_to_utf8(devcard::path_from_utf8(emoji)) == emoji);

  {
    devcard::Db db(dir / "devicedb.db");
    devcard::migrate(db);
    const devcard::SeedResult seed = devcard::seed_default_users(db, dir);
    CHECK(seed.seeded);

    // 2) 种子文件名的码元必须与常量逐码点一致。
    CHECK(devcard::path_to_utf8(seed.password_file.filename()) == expected);

    // 3) 再对着枚举出来的目录项核一遍：这是「落盘」而不是「内存里看着对」。
    bool found = false;
    bool mojibake = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      const std::string name = devcard::path_to_utf8(entry.path().filename());
      if (name == expected) found = true;
      if (name.find("初始账号密码.txt") == std::string::npos &&
          name.find(".txt") != std::string::npos) {
        mojibake = true;
      }
    }
    CHECK(found);
    CHECK(!mojibake);
  }

  // 4) 删除也要按同一套码元命中，不能删出个「看起来删了」。
  devcard::remove_seed_password_file(dir);
  CHECK(!std::filesystem::exists(dir / devcard::path_from_utf8(expected)));
}

// 场景函数：Db 是局部对象，函数返回即析构，调用方随后删目录才不会撞上打开的库。
void run_legacy_import_case(const std::filesystem::path& dir) {
  const std::filesystem::path legacy_dir = dir / "legacy";
  std::filesystem::create_directories(legacy_dir);
  const std::filesystem::path legacy_file = legacy_dir / "devicedb.db";
  {
  sqlite3* conn = nullptr;
  // SQLite 的路径参数是 UTF-8；Windows 上 .string() 给的是 ACP，
  // 临时目录落在中文用户名下时会打不开，所以走 path_to_utf8。
  sqlite3_open(devcard::path_to_utf8(legacy_file).c_str(), &conn);
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

}

// 场景函数：Db 是局部对象，函数返回即析构，调用方随后删目录才不会撞上打开的库。
void run_company_name_case(const std::filesystem::path& dir) {
  devcard::Db db(dir / "devicedb.db");
  devcard::migrate(db);

  CHECK(devcard::company_name(db) ==
        std::string(devcard::kDefaultCompanyName));
  CHECK(devcard::company_print_name(db) ==
        std::string(devcard::kDefaultCompanyName));
  CHECK(devcard::company_name_width("安业AB") == 3.0);
  CHECK(devcard::company_name_width("") == 0.0);

  db.write([](sqlite3* conn) {
    devcard::set_setting(conn, "print_company_name",
                         "深圳市安业物业管理有限公司南山分公司");
  });
  CHECK(devcard::company_name_width(devcard::company_name(db)) == 18.0);
  CHECK(devcard::company_print_name(db) == "深圳市安业物业管理有限公司");

  // 录入上限 16 个字符：存得下，卡面仍按 13 字宽截断。
  CHECK(devcard::kCompanyNameInputMaxLength == 16);
  db.write([](sqlite3* conn) {
    devcard::set_setting(conn, "print_company_name",
                         "深圳市安业物业管理有限公司南山分");
  });
  CHECK(devcard::company_name_width(devcard::company_name(db)) == 16.0);
  CHECK(devcard::company_print_name(db) == "深圳市安业物业管理有限公司");

  db.write([](sqlite3* conn) {
    devcard::set_setting(conn, "print_company_name",
                         "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
  });
  CHECK(devcard::company_name_width(devcard::company_name(db)) == 13.0);
  CHECK(devcard::company_print_name(db) == "ABCDEFGHIJKLMNOPQRSTUVWXYZ");

}

// 场景函数：Db 是局部对象，函数返回即析构，调用方随后删目录才不会撞上打开的库。
void run_xlsx_template_case(const std::filesystem::path& dir) {
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

}

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
    // Argon2id：编码串、正确/错误密码。
    const std::string hash = devcard::hash_password("Passw0rd!");
    CHECK(hash.rfind("$argon2id$", 0) == 0);
    CHECK(devcard::verify_password(hash, "Passw0rd!"));
    CHECK(!devcard::verify_password(hash, "passw0rd!"));
    CHECK(!devcard::verify_password("", "Passw0rd!"));
  }

  {
    const std::filesystem::path dir = temp_db().parent_path();
    run_user_service_case(dir);
    check_db_closed(dir);
    std::filesystem::remove_all(dir);
    CHECK(!std::filesystem::exists(dir));
  }

  {
    // 中文路径：种子文件名按「磁盘上的目录项码元」比，不看界面显示。
    const std::filesystem::path dir = temp_db().parent_path();
    run_path_encoding_case(dir);
    check_db_closed(dir);
    std::filesystem::remove_all(dir);
    CHECK(!std::filesystem::exists(dir));
  }

  {
    // 旧库：13 条设备 + 3 个用户（含默认账号与超长字段）。
    const std::filesystem::path dir = temp_db().parent_path();
    run_legacy_import_case(dir);
    check_db_closed(dir);
    std::filesystem::remove_all(dir);
    CHECK(!std::filesystem::exists(dir));
  }

  {
    // 公司名：默认值回落；自定义按 13 个显示宽度截断（汉字 1，ASCII 0.5）。
    const std::filesystem::path dir = temp_db().parent_path();
    run_company_name_case(dir);
    check_db_closed(dir);
    std::filesystem::remove_all(dir);
    CHECK(!std::filesystem::exists(dir));
  }

  {
    // 自更新阶段一：地址白名单、文件名清洗、双校验与失败清理。
    const std::filesystem::path dir = temp_db().parent_path();
    run_update_package_case(dir);
    std::filesystem::remove_all(dir);
    CHECK(!std::filesystem::exists(dir));
  }

  {
    // 导入模板：第一个工作表只有表头，且与导入校验共用同一份常量。
    const std::filesystem::path dir = temp_db().parent_path();
    run_xlsx_template_case(dir);
    check_db_closed(dir);
    std::filesystem::remove_all(dir);
    CHECK(!std::filesystem::exists(dir));
  }

  std::filesystem::remove_all(file.parent_path());
  if (failures) {
    std::cerr << failures << " 项失败\n";
    return 1;
  }
  std::cout << "core tests passed\n";
  return 0;
}
