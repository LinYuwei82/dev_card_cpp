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

 private:
  friend std::unique_ptr<ServerHandle> start_server(const ServerOptions&);

  struct Impl;
  std::unique_ptr<Impl> impl_;
  int port_ = 0;
};

std::unique_ptr<ServerHandle> start_server(const ServerOptions& options);

}  // namespace devcard
