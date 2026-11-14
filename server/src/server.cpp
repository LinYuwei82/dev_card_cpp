#include "devcard/server.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <thread>

#include "devcard/db.h"
#include "devcard/error.h"
#include "devcard/migrations.h"
#include "web_assets.h"

namespace devcard {

struct ServerHandle::Impl {
  httplib::Server server;
  std::thread thread;
  std::unique_ptr<Db> db;
};

ServerHandle::ServerHandle() : impl_(std::make_unique<Impl>()) {}
ServerHandle::~ServerHandle() {
  if (impl_ && impl_->server.is_running()) impl_->server.stop();
  if (impl_ && impl_->thread.joinable()) impl_->thread.join();
}

namespace {

std::string json_body(const nlohmann::json& value) { return value.dump(); }

void set_json(httplib::Response& response, const std::string& body, int status = 200) {
  response.status = status;
  response.set_header("Content-Type", "application/json; charset=utf-8");
  response.set_header("Cache-Control", "no-store");
  response.body = body;
}

}  // namespace

std::unique_ptr<ServerHandle> start_server(const ServerOptions& options) {
  auto handle = std::unique_ptr<ServerHandle>(new ServerHandle());
  auto& impl = *handle->impl_;

  impl.db = std::make_unique<Db>(options.data_dir / "devicedb.db");
  migrate(*impl.db);
  const auto schema = impl.db->schema_version();
  const std::string version = DEVCARD_VERSION;

  impl.server.Get("/api/health", [schema, version](const httplib::Request&,
                                                   httplib::Response& response) {
    set_json(response,
             json_body({{"status", "ok"},
                        {"version", version},
                        {"schema_version", schema},
                        {"mode", "local"}}));
  });

  impl.server.Get("/", [](const httplib::Request&, httplib::Response& response) {
    response.set_header("Content-Type", "text/html; charset=utf-8");
    response.body = embedded_index_html();
  });

  impl.server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
    if (response.status == 404) {
      set_json(response, json_body({{"error", "not_found"}}), 404);
    }
  });

  if (!impl.server.bind_to_port(options.host, options.port)) {
    throw AppError("port_in_use",
                   "无法监听 " + options.host + ":" + std::to_string(options.port));
  }
  handle->port_ = options.port;
  // bind_to_port 已成功；listen_after_bind 只处理请求。
  impl.thread = std::thread([&impl] { impl.server.listen_after_bind(); });
  return handle;
}

}  // namespace devcard
