#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#endif

#include "config.h"
#include "devcard/backup.h"
#include "devcard/db.h"
#include "devcard/error.h"
#include "devcard/legacy.h"
#include "devcard/migrations.h"
#include "devcard/server.h"
#include "devcard/settings.h"
#include "devcard/service/user_service.h"

namespace {

std::string trim_line(std::string line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
    line.pop_back();
  }
  return line;
}

std::string prompt_password(const std::string& label) {
  std::cout << label << std::flush;
#ifndef _WIN32
  termios old_settings{};
  const bool tty = ::isatty(STDIN_FILENO) != 0;
  if (tty) {
    ::tcgetattr(STDIN_FILENO, &old_settings);
    termios settings = old_settings;
    settings.c_lflag &= static_cast<tcflag_t>(~ECHO);
    ::tcsetattr(STDIN_FILENO, TCSANOW, &settings);
  }
#endif
  std::string line;
  std::getline(std::cin, line);
#ifndef _WIN32
  if (tty) {
    ::tcsetattr(STDIN_FILENO, TCSANOW, &old_settings);
    std::cout << "\n";
  }
#endif
  return trim_line(line);
}

struct OpenedDatabase {
  std::unique_ptr<devcard::Db> db;
  devcard::LegacyImportReport legacy;
};

OpenedDatabase open_database(const std::filesystem::path& data_dir) {
  OpenedDatabase opened;
  opened.legacy = devcard::auto_import_if_needed(data_dir);
  opened.db = std::make_unique<devcard::Db>(data_dir / "devicedb.db");
  devcard::migrate(*opened.db);
  devcard::seed_default_users(*opened.db, data_dir);
  return opened;
}

// 只读统计目标库的设备数：这道闸本身绝不能改动已有台账（连 WAL 检查点都不做），
// 所以不用 Db（读写连接），直接 SQLITE_OPEN_READONLY。
std::int64_t count_devices_readonly(const std::filesystem::path& file) {
  sqlite3* conn = nullptr;
  // SQLite 的路径参数必须是 UTF-8；path::string() 在 Windows 是 ACP（GBK），
  // 中文目录会被解释成错误路径，这里显式转 UTF-8。
  const auto u8 = file.u8string();
  const std::string native(reinterpret_cast<const char*>(u8.data()), u8.size());
  if (sqlite3_open_v2(native.c_str(), &conn, SQLITE_OPEN_READONLY,
                      nullptr) != SQLITE_OK) {
    if (conn) sqlite3_close(conn);
    return -1;
  }
  std::int64_t total = -1;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(conn, "SELECT COUNT(*) FROM device", -1, &stmt,
                         nullptr) == SQLITE_OK &&
      stmt && sqlite3_step(stmt) == SQLITE_ROW) {
    total = sqlite3_column_int64(stmt, 0);
  }
  if (stmt) sqlite3_finalize(stmt);
  sqlite3_close(conn);
  return total;
}

void print_legacy_report(const devcard::LegacyImportReport& report);

// 安装版首启：exe 同级 data\（或旧 bin\）若带着 devicedb.db，导入用户目录。
// 原文件只读、不改名、不删；用户目录已有台账时拒绝，绝不合并两份台账。
void import_adjacent_legacy(const std::filesystem::path& data_dir) {
  if (!devcard::is_installed_layout()) return;
  const std::filesystem::path exe = devcard::executable_dir();
  std::error_code ec;
  std::filesystem::path source;
  for (const auto& candidate :
       {exe / "data" / "devicedb.db", exe / "bin" / "devicedb.db"}) {
    if (std::filesystem::exists(candidate, ec)) {
      source = candidate;
      break;
    }
  }
  if (source.empty()) return;

  const devcard::LegacyInfo info = devcard::inspect_legacy(source);
  if (!info.is_legacy) {
    std::cout << "提示：exe 同级发现 " << source.string()
              << "，不是旧版库，未自动导入。" << std::endl;
    return;
  }

  const std::filesystem::path target = data_dir / "devicedb.db";
  std::int64_t existing = 0;
  bool target_is_legacy = false;
  if (std::filesystem::exists(target, ec)) {
    target_is_legacy = devcard::inspect_legacy(target).is_legacy;
    if (!target_is_legacy) {
      existing = count_devices_readonly(target);
      if (existing < 0) {
        throw devcard::AppError(
            "legacy_conflict",
            "无法读取用户数据目录里的台账记录数，拒绝自动导入：\n  目标: " +
                target.string() +
                "\n请先用 --data-dir 指向正确目录，或人工检查该库是否损坏。");
      }
      if (existing > 0) {
        throw devcard::AppError(
            "legacy_conflict",
            "用户数据目录已有台账，拒绝自动导入（不会合并两份台账）：\n"
            "  已有: " + target.string() + "（设备 " +
                std::to_string(existing) + " 条）\n"
            "  待导入: " + source.string() + "（设备 " +
                std::to_string(info.devices) + " 条）\n"
            "请确认保留哪一份：用 --data-dir 指向旧目录，或先自行备份/移走其中一个库。");
      }
    }
  }
  if (target_is_legacy) return;  // 由 open_database 的 auto_import_if_needed 处理

  std::cout << "检测到安装版随包旧数据 " << source.string()
            << "，导入到用户数据目录（原文件保留、不改名）：" << std::endl;
  print_legacy_report(devcard::import_legacy(source, data_dir));
}

void print_legacy_report(const devcard::LegacyImportReport& report) {
  if (!report.performed) {
    std::cout << "未发现需要导入的旧库" << std::endl;
    return;
  }
  std::cout << "旧库导入完成\n"
            << "  来源: " << report.source << "\n"
            << "  SHA256: " << report.source_sha256 << "\n"
            << "  备份: " << report.backup << "\n"
            << "  归档: "
            << (report.archived_as.empty() ? "(源库保留原处)" : report.archived_as)
            << "\n"
            << "  设备: 共 " << report.devices_total << "，导入 "
            << report.devices_imported << "，跳过 " << report.devices_skipped
            << "\n"
            << "  用户: 共 " << report.users_total << "，导入 "
            << report.users_imported << "，跳过 " << report.users_skipped
            << std::endl;
  for (const std::string& warning : report.warnings) {
    std::cout << "  警告: " << warning << std::endl;
  }
}

void print_usage() {
  std::cout
      << "devcard-server [选项]\n"
      << "  --server               以网络版启动：允许内网浏览器访问\n"
      << "  --listen IP            监听地址（默认自动选一块私有 IPv4）\n"
      << "  --port N               监听端口（默认 18080）\n"
      << "  --allow-any            允许 --listen 0.0.0.0（默认拒绝）\n"
      << "  --data-dir PATH        数据目录（命令行 → 环境变量 → 配置文件 → 形态默认）\n"
      << "  --config PATH          配置文件（默认 exe 同目录 devcard-server.json）\n"
      << "  --import-legacy PATH   导入旧版 devicedb.db 后退出\n"
      << "  --set-password USER    重置本机账号密码后退出（密码从标准输入读取）\n"
      << "  --backup               立即备份后退出\n"
      << "  --restore NAME         从 backups/ 下的备份恢复后退出\n"
      << "  --version              输出版本\n"
      << "  --help                 显示本帮助\n";
}

int parse_port_arg(const std::string& text) {
  try {
    const std::size_t used = text.find_first_not_of("0123456789");
    if (used != std::string::npos) throw std::invalid_argument("port");
    const int port = std::stoi(text);
    if (port < 1 || port > 65535) throw std::out_of_range("port");
    return port;
  } catch (const std::exception&) {
    throw devcard::AppError("bad_config",
                            "--port 必须是 1–65535 的整数，收到: " + text);
  }
}

}  // namespace

int main(int argc, char** argv) {
  devcard::CliOverrides cli;
  std::string import_legacy_path;
  std::string set_password_user;
  std::string restore_name;
  bool backup_only = false;
  bool show_version = false;
  bool server_mode = false;

  try {
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      auto next = [&](const char* name) -> std::string {
        if (i + 1 >= argc) {
          throw devcard::AppError("bad_config",
                                  std::string(name) + " 缺少参数");
        }
        return argv[++i];
      };
      if (arg == "--server") {
        server_mode = true;
      } else if (arg == "--allow-any") {
        cli.allow_any = true;
      } else if (arg == "--port") {
        cli.port = parse_port_arg(next("--port"));
      } else if (arg == "--listen") {
        cli.listen = next("--listen");
      } else if (arg == "--config") {
        cli.config = std::filesystem::path(next("--config"));
      } else if (arg == "--data-dir") {
        cli.data_dir = next("--data-dir");
      } else if (arg == "--import-legacy") {
        import_legacy_path = next("--import-legacy");
      } else if (arg == "--set-password") {
        set_password_user = next("--set-password");
      } else if (arg == "--restore") {
        restore_name = next("--restore");
      } else if (arg == "--backup") {
        backup_only = true;
      } else if (arg == "--version") {
        show_version = true;
      } else if (arg == "--help" || arg == "-h") {
        print_usage();
        return 0;
      } else {
        std::cerr << "未知参数: " << arg << "\n";
        return 2;
      }
    }

    if (show_version) {
      std::cout << DEVCARD_VERSION << std::endl;
      return 0;
    }

    const devcard::ServerConfig config = devcard::resolve_config(cli);

    if (!import_legacy_path.empty()) {
      const devcard::LegacyImportReport report =
          devcard::import_legacy(import_legacy_path, config.data_dir);
      print_legacy_report(report);
      return 0;
    }

    if (!set_password_user.empty()) {
      OpenedDatabase opened = open_database(config.data_dir);
      devcard::UserService users(*opened.db);
      const std::string first = prompt_password("新密码（8–128 个字符）: ");
      const std::string second = prompt_password("再次输入新密码: ");
      if (first != second) {
        std::cerr << "两次输入不一致，未修改" << std::endl;
        return 1;
      }
      devcard::validate_password(first);
      bool exists = true;
      try {
        users.get(set_password_user);
      } catch (const devcard::AppError&) {
        exists = false;
      }
      if (exists) {
        users.reset_password(set_password_user, first);
      } else {
        users.create(set_password_user, first, set_password_user, "admin");
        std::cout << "账号不存在，已创建管理员账号 " << set_password_user << std::endl;
      }
      std::cout << "已重置 " << set_password_user
                << " 的密码，下次登录必须修改" << std::endl;
      return 0;
    }

    if (backup_only) {
      OpenedDatabase opened = open_database(config.data_dir);
      const devcard::BackupEntry entry =
          devcard::create_backup(*opened.db, config.data_dir, "cli");
      std::cout << "备份完成: " << entry.path << " (" << entry.bytes << " 字节)"
                << std::endl;
      return 0;
    }

    if (!restore_name.empty()) {
      OpenedDatabase opened = open_database(config.data_dir);
      devcard::restore_backup(*opened.db, config.data_dir, restore_name);
      std::cout << "已从备份 " << restore_name << " 恢复（恢复前已自动备份当前库）"
                << std::endl;
      return 0;
    }

    // 监听地址：显式配置优先；网络模式未指定时自动选一块私有 IPv4，
    // 本地窗口模式（不带 --server）固定回环。
    std::string listen = config.listen;
    std::string listen_source = config.listen_source;
    if (listen.empty()) {
      if (server_mode) {
        listen = devcard::select_listen_address(config);
        listen_source = "自动选择私有网卡";
      } else {
        listen = "127.0.0.1";
        listen_source = "默认（本机窗口模式）";
      }
    } else if (!server_mode && !devcard::is_private_ipv4(listen) &&
               listen != "127.0.0.1" && listen != "localhost") {
      throw devcard::AppError(
          "bad_config",
          "监听地址 " + listen + " 需要配合 --server 使用；本地模式只监听回环。");
    }

    if (devcard::is_wildcard_listen(listen)) {
      if (!config.allow_any) {
        throw devcard::AppError(
            "bad_listen",
            "拒绝监听 " + listen +
                "：这会对所有网卡开放。确认要这样做时，请同时显式加 --allow-any。");
      }
      std::cerr << "警告：监听 " << listen
                << " 将对所有网卡开放，请确保处在可信内网。" << std::endl;
    }

    // 安装版首启：先处理 exe 同级的旧库，再打开正式数据库。
    import_adjacent_legacy(config.data_dir);

    devcard::ServerOptions options;
    options.data_dir = config.data_dir;
    options.host = listen;
    options.port = config.port;
    options.network = server_mode;
    options.allowed_hosts = config.allowed_hosts;

    auto server = devcard::start_server(options);

    std::cout << "DevCard " << (server_mode ? "网络版" : "本地版") << " "
              << DEVCARD_VERSION << "（schema "
              << devcard::target_schema_version() << "）\n";
    if (config.config_loaded) {
      std::cout << "配置文件: " << config.config_file.string() << "（来源："
                << config.config_source << "）\n";
    } else {
      std::cout << "配置文件: (未使用，exe 同目录无 devcard-server.json)\n";
    }
    std::cout << "数据目录: " << config.data_dir.string() << "（来源："
              << config.data_dir_source << "）\n"
              << "安装形态: " << (devcard::is_installed_layout() ? "安装版" : "便携版")
              << "\n"
              << "数据库:   " << (config.data_dir / "devicedb.db").string() << "\n"
              << "监听地址: " << listen << ":" << server->port() << "（来源："
              << listen_source << "，端口来源：" << config.port_source << "）\n";
    if (server_mode) {
      std::cout << "访问地址: http://" << listen << ":" << server->port() << "/"
                << std::endl;
    } else {
      std::cout << "访问地址: http://127.0.0.1:" << server->port() << "/"
                << std::endl;
    }
    std::cout.flush();

    while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
  } catch (const devcard::AppError& error) {
    std::cerr << "启动失败: " << error.what() << std::endl;
    if (error.code() == "instance_in_use") return 3;
    if (error.code() == "bad_config" || error.code() == "bad_listen") return 2;
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "启动失败: " << error.what() << std::endl;
    return 1;
  }
  return 0;
}
