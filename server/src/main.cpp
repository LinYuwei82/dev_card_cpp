#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#endif

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
      << "  --port N               监听端口（默认 18080）\n"
      << "  --data-dir PATH        数据目录（默认为系统应用数据目录）\n"
      << "  --import-legacy PATH   导入旧版 devicedb.db 后退出\n"
      << "  --set-password USER    重置本机账号密码后退出（密码从标准输入读取）\n"
      << "  --backup               立即备份后退出\n"
      << "  --restore NAME         从 backups/ 下的备份恢复后退出\n"
      << "  --version              输出版本\n"
      << "  --help                 显示本帮助\n";
}

}  // namespace

int main(int argc, char** argv) {
  devcard::ServerOptions options;
  options.data_dir = devcard::default_data_dir();

  std::string import_legacy_path;
  std::string set_password_user;
  std::string restore_name;
  bool backup_only = false;
  bool show_version = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << name << " 缺少参数\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--port") {
      options.port = std::stoi(next("--port"));
    } else if (arg == "--data-dir") {
      options.data_dir = next("--data-dir");
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

  try {
    if (show_version) {
      std::cout << DEVCARD_VERSION << std::endl;
      return 0;
    }

    if (!import_legacy_path.empty()) {
      const devcard::LegacyImportReport report =
          devcard::import_legacy(import_legacy_path, options.data_dir);
      print_legacy_report(report);
      return 0;
    }

    if (!set_password_user.empty()) {
      OpenedDatabase opened = open_database(options.data_dir);
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
      OpenedDatabase opened = open_database(options.data_dir);
      const devcard::BackupEntry entry =
          devcard::create_backup(*opened.db, options.data_dir, "cli");
      std::cout << "备份完成: " << entry.path << " (" << entry.bytes << " 字节)"
                << std::endl;
      return 0;
    }

    if (!restore_name.empty()) {
      OpenedDatabase opened = open_database(options.data_dir);
      devcard::restore_backup(*opened.db, options.data_dir, restore_name);
      std::cout << "已从备份 " << restore_name << " 恢复（恢复前已自动备份当前库）"
                << std::endl;
      return 0;
    }

    auto server = devcard::start_server(options);
    std::cout << "devcard-server listening http://" << options.host << ":"
              << server->port() << " data=" << options.data_dir << std::endl;
    while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
  } catch (const std::exception& error) {
    std::cerr << "启动失败: " << error.what() << std::endl;
    return 1;
  }
  return 0;
}
