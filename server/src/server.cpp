#include "devcard/server.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <exception>
#include <string>
#include <thread>

#include "devcard/db.h"
#include "devcard/error.h"
#include "devcard/migrations.h"
#include "devcard/models.h"
#include "devcard/service/device_service.h"
#include "devcard/xlsx.h"
#include "web_assets.h"

namespace devcard {

struct ServerHandle::Impl {
  httplib::Server server;
  std::thread thread;
  std::unique_ptr<Db> db;
  std::unique_ptr<DeviceService> devices;
};

ServerHandle::ServerHandle() : impl_(std::make_unique<Impl>()) {}
ServerHandle::~ServerHandle() {
  if (impl_ && impl_->server.is_running()) impl_->server.stop();
  if (impl_ && impl_->thread.joinable()) impl_->thread.join();
}

namespace {

using nlohmann::json;

std::string json_body(const json& value) { return value.dump(); }

void set_json(httplib::Response& response, const std::string& body, int status = 200) {
  response.status = status;
  response.set_header("Content-Type", "application/json; charset=utf-8");
  response.set_header("Cache-Control", "no-store");
  response.body = body;
}

void set_json(httplib::Response& response, const json& value, int status = 200) {
  set_json(response, json_body(value), status);
}

void set_asset(httplib::Response& response, const char* body, const char* type) {
  response.status = 200;
  response.set_header("Content-Type", type);
  response.set_header("Cache-Control", "no-store");
  response.body = body;
}

json device_json(const Device& device) {
  return {{"id", device.id},
          {"dev_name", device.dev_name},
          {"location", device.location},
          {"control_range", device.control_range},
          {"phone", device.phone},
          {"created_at", device.created_at},
          {"updated_at", device.updated_at}};
}

Device device_from_json(const json& body) {
  Device device;
  device.dev_name = body.value("dev_name", "");
  device.location = body.value("location", "");
  device.control_range = body.value("control_range", "");
  device.phone = body.value("phone", "");
  return device;
}

int int_param(const httplib::Request& request, const char* name, int fallback) {
  const std::string value = request.get_param_value(name);
  if (value.empty()) return fallback;
  try {
    return std::stoi(value);
  } catch (...) {
    return fallback;
  }
}

DeviceQuery query_from(const httplib::Request& request) {
  DeviceQuery query;
  query.keyword = request.get_param_value("keyword");
  query.field = request.get_param_value("field");
  query.name = request.get_param_value("name");
  query.location = request.get_param_value("location");
  query.page = int_param(request, "page", 0);
  query.page_size = int_param(request, "page_size", 20);
  query.sort = request.get_param_value("sort");
  if (query.sort.empty()) query.sort = "id";
  query.desc = request.get_param_value("desc") == "1";
  return query;
}

int status_for(const AppError& error) {
  if (error.code() == "duplicate") return 409;
  if (error.code() == "not_found") return 404;
  if (error.code() == "legacy_db") return 409;
  if (error.code() == "invalid_device" || error.code() == "bad_field" ||
      error.code() == "bad_json" || error.code() == "bad_header" ||
      error.code() == "bad_xlsx")
    return 400;
  return 500;
}

}  // namespace

std::unique_ptr<ServerHandle> start_server(const ServerOptions& options) {
  auto handle = std::unique_ptr<ServerHandle>(new ServerHandle());
  auto& impl = *handle->impl_;

  impl.db = std::make_unique<Db>(options.data_dir / "devicedb.db");
  migrate(*impl.db);
  impl.devices = std::make_unique<DeviceService>(*impl.db);
  const auto schema = impl.db->schema_version();
  const std::string version = DEVCARD_VERSION;

  impl.server.Get("/api/health", [schema, version](const httplib::Request&,
                                                   httplib::Response& response) {
    set_json(response, json{{"status", "ok"},
                            {"version", version},
                            {"schema_version", schema},
                            {"mode", "local"}});
  });

  impl.server.Get("/api/devices", [&impl](const httplib::Request& request,
                                          httplib::Response& response) {
    const DevicePage page = impl.devices->list(query_from(request));
    json items = json::array();
    for (const Device& device : page.items) items.push_back(device_json(device));
    set_json(response, json{{"items", items},
                            {"total", page.total},
                            {"page", page.page},
                            {"page_size", page.page_size}});
  });

  impl.server.Get("/api/devices/options",
                  [&impl](const httplib::Request&, httplib::Response& response) {
                    const DeviceOptions values = impl.devices->options();
                    set_json(response, json{{"names", values.names},
                                            {"locations", values.locations}});
                  });

  impl.server.Post("/api/devices/import",
                   [&impl](const httplib::Request& request,
                           httplib::Response& response) {
                     const auto rows = xlsx::read(request.body);
                     const ImportReport report = impl.devices->import_rows(rows);
                     set_json(response, json{{"imported", report.imported},
                                             {"skipped_empty", report.skipped_empty},
                                             {"duplicates", report.duplicates},
                                             {"truncated", report.truncated}});
                   });

  impl.server.Get("/api/devices/export",
                  [&impl](const httplib::Request&, httplib::Response& response) {
                    const std::string body =
                        xlsx::write(impl.devices->export_rows());
                    response.status = 200;
                    response.set_header(
                        "Content-Type",
                        "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
                    response.set_header(
                        "Content-Disposition",
                        "attachment; filename=\"devices.xlsx\"; "
                        "filename*=UTF-8''%E8%AE%BE%E5%A4%87%E5%8F%B0%E8%B4%A6.xlsx");
                    response.set_header("Cache-Control", "no-store");
                    response.body = body;
                  });

  impl.server.Get(R"(/api/devices/(\d+))",
                  [&impl](const httplib::Request& request,
                          httplib::Response& response) {
                    const auto id = std::stoll(request.matches[1].str());
                    set_json(response, device_json(impl.devices->get(id)));
                  });

  impl.server.Post("/api/devices", [&impl](const httplib::Request& request,
                                           httplib::Response& response) {
    Device device;
    try {
      device = device_from_json(json::parse(request.body));
    } catch (const json::exception&) {
      throw AppError("bad_json", "请求体不是合法 JSON");
    }
    set_json(response, device_json(impl.devices->create(std::move(device))), 201);
  });

  impl.server.Put(R"(/api/devices/(\d+))",
                  [&impl](const httplib::Request& request,
                          httplib::Response& response) {
                    const auto id = std::stoll(request.matches[1].str());
                    Device device;
                    try {
                      device = device_from_json(json::parse(request.body));
                    } catch (const json::exception&) {
                      throw AppError("bad_json", "请求体不是合法 JSON");
                    }
                    set_json(response, device_json(impl.devices->update(id, std::move(device))));
                  });

  impl.server.Delete(R"(/api/devices/(\d+))",
                     [&impl](const httplib::Request& request,
                             httplib::Response& response) {
                       impl.devices->remove(std::stoll(request.matches[1].str()));
                       set_json(response, json{{"ok", true}});
                     });

  impl.server.Get("/", [](const httplib::Request&, httplib::Response& response) {
    set_asset(response, embedded_index_html(), "text/html; charset=utf-8");
  });
  impl.server.Get("/styles.css", [](const httplib::Request&, httplib::Response& response) {
    set_asset(response, embedded_styles_css(), "text/css; charset=utf-8");
  });
  impl.server.Get("/app.js", [](const httplib::Request&, httplib::Response& response) {
    set_asset(response, embedded_app_js(), "application/javascript; charset=utf-8");
  });

  impl.server.set_error_handler([](const httplib::Request&, httplib::Response& response) {
    // 业务层已经写好 JSON 的错误体，只有路由未命中时才补默认内容。
    if (response.status == 404 && response.body.empty()) {
      set_json(response, json{{"error", "not_found"}, {"message", "接口不存在"}}, 404);
    }
  });

  impl.server.set_exception_handler([](const httplib::Request&,
                                       httplib::Response& response,
                                       std::exception_ptr error) {
    try {
      std::rethrow_exception(error);
    } catch (const AppError& app_error) {
      set_json(response,
               json{{"error", app_error.code()}, {"message", app_error.what()}},
               status_for(app_error));
    } catch (const std::exception& other) {
      set_json(response, json{{"error", "internal"}, {"message", other.what()}}, 500);
    } catch (...) {
      set_json(response, json{{"error", "internal"}, {"message", "未知错误"}}, 500);
    }
  });

  if (!impl.server.bind_to_port(options.host, options.port)) {
    throw AppError("port_in_use",
                   "无法监听 " + options.host + ":" + std::to_string(options.port));
  }
  handle->port_ = options.port;
  impl.thread = std::thread([&impl] { impl.server.listen_after_bind(); });
  return handle;
}

}  // namespace devcard
