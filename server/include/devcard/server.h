#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace devcard {

std::filesystem::path default_data_dir();

// 安装形态：Inno 安装版（程序目录有 unins000.exe，或位于
// %LOCALAPPDATA%\Programs\DevCard）返回 true；绿色版返回 false。
bool is_installed_layout();

// 可执行文件所在目录：配置文件、随包字体都相对它解析，不依赖当前工作目录。
std::filesystem::path executable_dir();

// 随包分发的子集字体目录（web/fonts 在打包时复制到 app 的 Resources/fonts；
// Windows 绿色版放在 exe 旁的 fonts/）。可用 DEVCARD_FONT_DIR 覆盖。
std::filesystem::path default_font_dir();

struct ServerOptions {
  std::filesystem::path data_dir;
  std::string host = "127.0.0.1";
  int port = 18080;
  // --server 网络模式：允许内网来源、不注入本地自动登录令牌、
  // Host 白名单加入本机私有 IP 与 allowed_hosts。
  bool network = false;
  std::vector<std::string> allowed_hosts;
};

class ServerHandle {
 public:
  ServerHandle();
  ~ServerHandle();

  ServerHandle(const ServerHandle&) = delete;
  ServerHandle& operator=(const ServerHandle&) = delete;

  int port() const { return port_; }

  // 进程级一次性本地会话令牌：壳通过页面注入的 meta 换取会话。
  const std::string& local_token() const;

 private:
  friend std::unique_ptr<ServerHandle> start_server(const ServerOptions&);

  struct Impl;
  std::unique_ptr<Impl> impl_;
  int port_ = 0;
};

std::unique_ptr<ServerHandle> start_server(const ServerOptions& options);

// 错误码 → HTTP 状态码的唯一来源。路由里 throw AppError(code, ...) 的 code
// 必须在这张表里有对应项；缺了会落 500，把本该 4xx 的用户错误报成服务端错误。
bool http_status_known(std::string_view code);
// 未登记时返回 500（并应由「清单与表一致」的测试提前拦下）。
int http_status_for(std::string_view code);
// 路由实际会抛出的错误码清单（服务启动阶段的错误码不在其中）。
const std::vector<std::string>& route_error_codes();
// 映射表里登记的全部错误码（用于反向检查：表里不许有孤儿项）。
const std::vector<std::string>& mapped_error_codes();

}  // namespace devcard
