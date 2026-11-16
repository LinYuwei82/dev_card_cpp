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

// 登录与改密的失败限流。只管这两条接口，不做全局限流：
// 局域网里先被打死的是静态页与轮询，全局限流会连带把正常界面拖垮。
struct RateLimitOptions {
  bool enabled = true;
  int max_failures = 5;      // 同键连续失败次数阈值，达到即锁
  int window_seconds = 300;  // 计数窗口，超出窗口重新计数
  int lock_seconds = 60;     // 锁定时长，期间一律 429
};

struct ServerOptions {
  std::filesystem::path data_dir;
  std::string host = "127.0.0.1";
  int port = 18080;
  // --server 网络模式：允许内网来源、不注入本地自动登录令牌、
  // Host 白名单加入本机私有 IP 与 allowed_hosts。
  bool network = false;
  std::vector<std::string> allowed_hosts;
  RateLimitOptions rate_limit;
  // 开发态前端目录：非空时不使用编译期内嵌资源，每次请求直读磁盘。
  // 改页面/样式/脚本后刷新即可生效，不必重新 cmake --build。
  std::filesystem::path web_dir;
};

class ServerHandle {
 public:
  ServerHandle();
  ~ServerHandle();

  ServerHandle(const ServerHandle&) = delete;
  ServerHandle& operator=(const ServerHandle&) = delete;

  int port() const { return port_; }

  // 自更新阶段二：安装接口先回 202，再置位该标志让 main 正常退出
  // （停服由 ServerHandle 析构完成，安装器随后才拿到被占用的程序文件）。
  bool stop_requested() const;

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
