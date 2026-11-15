#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace devcard {

// 命令行覆盖项：只有显式给出的项才参与「命令行最高优先级」。
struct CliOverrides {
  std::optional<std::filesystem::path> config;
  std::optional<std::string> data_dir;
  std::optional<std::string> listen;
  std::optional<int> port;
  bool allow_any = false;
};

// 解析后的运行配置。每个值都带上来源，启动日志逐项打印，
// 避免「为什么它用了这个路径」只能靠猜。
struct ServerConfig {
  std::filesystem::path config_file;
  std::string config_source;
  bool config_loaded = false;
  std::filesystem::path data_dir;
  std::string data_dir_source;
  std::string listen;
  std::string listen_source;
  int port = 18080;
  std::string port_source;
  bool allow_any = false;
  std::vector<std::string> allowed_hosts;
  // 登录/改密失败限流：默认开启。内存实现，重启清零（不写库，避免
  // 把「被锁」这种瞬时状态做成持久真值）。
  bool rate_limit_enabled = true;
  int rate_limit_max_failures = 5;
  int rate_limit_window_seconds = 300;
  int rate_limit_lock_seconds = 60;
  std::string rate_limit_source = "默认";
};

// 优先级：命令行 → 环境变量 → 配置文件 → 形态默认。
// 配置文件未知键直接报错（code=bad_config），不静默忽略。
// 数据目录的「形态默认」保留为 default_data_dir()，不在此处锁死
// %ProgramData% 或 %LOCALAPPDATA%（安装形态待拍板）。
ServerConfig resolve_config(const CliOverrides& cli);

// --server 模式未显式指定监听地址时自动选一块私有 IPv4：
// 恰好一块则返回它；零块或多块抛 AppError("bad_listen") 并列出候选。
std::string select_listen_address(const ServerConfig& config);

std::vector<std::string> private_ipv4_addresses();
std::string local_host_name();
bool is_private_ipv4(const std::string& address);
bool is_wildcard_listen(const std::string& address);

}  // namespace devcard
