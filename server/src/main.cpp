#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#endif

#include "config.h"
#include "apply_update.h"
#include "devcard/backup.h"
#include "devcard/db.h"
#include "devcard/error.h"
#include "devcard/paths.h"
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
};

OpenedDatabase open_database(const std::filesystem::path& data_dir) {
  OpenedDatabase opened;
  opened.db = std::make_unique<devcard::Db>(data_dir / "devicedb.db");
  devcard::migrate(*opened.db);
  devcard::seed_default_users(*opened.db, data_dir);
  return opened;
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
      << "  --set-password USER    重置本机账号密码后退出（密码从标准输入读取）\n"
      << "  --backup               立即备份后退出\n"
      << "  --restore NAME         从 backups/ 下的备份恢复后退出\n"
      << "  --apply-update PATH    内部：以更新协助进程运行（配合 --wait-pid/--result/--relaunch）\n"
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
  std::string set_password_user;
  std::string restore_name;
  bool backup_only = false;
  bool show_version = false;
  bool server_mode = false;
  // 自更新阶段二：协助器模式（由 --apply-update 进入），不解析运行配置。
  std::string apply_update_path;
  unsigned long apply_wait_pid = 0;
  std::string apply_result;
  std::string apply_relaunch;
  std::vector<std::string> apply_relaunch_args;

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
      } else if (arg == "--set-password") {
        set_password_user = next("--set-password");
      } else if (arg == "--restore") {
        restore_name = next("--restore");
      } else if (arg == "--backup") {
        backup_only = true;
      } else if (arg == "--apply-update") {
        apply_update_path = next("--apply-update");
      } else if (arg == "--wait-pid") {
        const std::string text = next("--wait-pid");
        char* end = nullptr;
        apply_wait_pid = std::strtoul(text.c_str(), &end, 10);
        if (!end || *end != '\0') {
          throw devcard::AppError("bad_config",
                                  "--wait-pid 必须是整数，收到: " + text);
        }
      } else if (arg == "--result") {
        apply_result = next("--result");
      } else if (arg == "--relaunch") {
        apply_relaunch = next("--relaunch");
      } else if (arg == "--relaunch-arg") {
        apply_relaunch_args.push_back(next("--relaunch-arg"));
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

    // 协助器模式：本进程不起服务，只等旧进程退出、跑安装器、写结果、拉起新版本。
    if (!apply_update_path.empty()) {
      devcard::ApplyUpdateOptions options;
      options.setup = devcard::path_from_utf8(apply_update_path);
      options.wait_pid = apply_wait_pid;
      options.result = devcard::path_from_utf8(apply_result);
      options.relaunch = devcard::path_from_utf8(apply_relaunch);
      options.relaunch_args = apply_relaunch_args;
      return devcard::run_apply_update(options);
    }

    const devcard::ServerConfig config = devcard::resolve_config(cli);

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

    devcard::ServerOptions options;
    options.data_dir = config.data_dir;
    options.host = listen;
    options.port = config.port;
    options.network = server_mode;
    options.allowed_hosts = config.allowed_hosts;
    options.rate_limit.enabled = config.rate_limit_enabled;
    options.rate_limit.max_failures = config.rate_limit_max_failures;
    options.rate_limit.window_seconds = config.rate_limit_window_seconds;
    options.rate_limit.lock_seconds = config.rate_limit_lock_seconds;

    auto server = devcard::start_server(options);

    std::cout << "DevCard " << (server_mode ? "网络版" : "本地版") << " "
              << DEVCARD_VERSION << "（schema "
              << devcard::target_schema_version() << "）\n";
    if (config.config_loaded) {
      std::cout << "配置文件: " << devcard::path_to_utf8(config.config_file) << "（来源："
                << config.config_source << "）\n";
    } else {
      std::cout << "配置文件: (未使用，exe 同目录无 devcard-server.json)\n";
    }
    std::cout << "数据目录: " << devcard::path_to_utf8(config.data_dir) << "（来源："
              << config.data_dir_source << "）\n"
              << "安装形态: " << (devcard::is_installed_layout() ? "安装版" : "便携版")
              << "\n"
              << "数据库:   " << devcard::path_to_utf8(config.data_dir / "devicedb.db") << "\n"
              << "监听地址: " << listen << ":" << server->port() << "（来源："
              << listen_source << "，端口来源：" << config.port_source << "）\n";
    std::cout << "登录限流: ";
    if (config.rate_limit_enabled) {
      std::cout << config.rate_limit_max_failures << " 次 / "
                << config.rate_limit_window_seconds << " 秒 → 锁 "
                << config.rate_limit_lock_seconds << " 秒（内存计数，重启清零）";
    } else {
      std::cout << "已关闭";
    }
    std::cout << "（来源：" << config.rate_limit_source << "）\n";
    if (server_mode) {
      std::cout << "访问地址: http://" << listen << ":" << server->port() << "/"
                << std::endl;
    } else {
      std::cout << "访问地址: http://127.0.0.1:" << server->port() << "/"
                << std::endl;
    }
    std::cout.flush();

    while (!server->stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // 安装接口已回 202：这里只负责把服务停干净，让协助进程接过程序目录。
    std::cout << "服务已停止，交由安装程序重启" << std::endl;
  } catch (const devcard::AppError& error) {
    // 机器码要单独打出来：判据靠它认「哪一类启动失败」，只有人读的说明没法断言。
    std::cerr << "启动失败[" << error.code() << "]: " << error.what()
              << std::endl;
    if (error.code() == "instance_in_use") return 3;
    if (error.code() == "bad_config" || error.code() == "bad_listen") return 2;
    return 1;
  } catch (const std::exception& error) {
    std::cerr << "启动失败: " << error.what() << std::endl;
    return 1;
  }
  return 0;
}
