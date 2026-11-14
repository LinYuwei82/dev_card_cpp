#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace devcard {

std::filesystem::path default_data_dir();

struct ServerOptions {
  std::filesystem::path data_dir;
  std::string host = "127.0.0.1";
  int port = 18080;
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

}  // namespace devcard
