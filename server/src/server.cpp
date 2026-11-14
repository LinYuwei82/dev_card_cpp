#include "devcard/server.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "devcard/backup.h"
#include "devcard/db.h"
#include "devcard/error.h"
#include "devcard/legacy.h"
#include "devcard/migrations.h"
#include "devcard/models.h"
#include "devcard/service/device_service.h"
#include "devcard/service/user_service.h"
#include "devcard/xlsx.h"
#include "web_assets.h"

namespace devcard {
namespace {

using nlohmann::json;

constexpr auto kSessionTtl = std::chrono::hours(12);

struct Session {
  std::string user;
  std::chrono::steady_clock::time_point last_seen;
};

struct ServerState {
  httplib::Server server;
  std::thread thread;
  std::unique_ptr<Db> db;
  std::unique_ptr<DeviceService> devices;
  std::unique_ptr<UserService> users;
  std::filesystem::path data_dir;
  std::string local_token;
  LegacyImportReport legacy;
  bool seeded = false;

  std::mutex sessions_mutex;
  std::map<std::string, Session> sessions;
};

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

void set_asset(httplib::Response& response, const std::string& body, const char* type) {
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

json user_json(const User& user) {
  return {{"user_name", user.user_name},
          {"display_name", user.display_name},
          {"role", user.role},
          {"must_change_password", user.must_change_password},
          {"locked_until", user.locked_until},
          {"created_at", user.created_at},
          {"updated_at", user.updated_at}};
}

Device device_from_json(const json& body) {
  Device device;
  device.dev_name = body.value("dev_name", "");
  device.location = body.value("location", "");
  device.control_range = body.value("control_range", "");
  device.phone = body.value("phone", "");
  return device;
}

json parse_body(const std::string& body) {
  try {
    return json::parse(body.empty() ? "{}" : body);
  } catch (const json::exception&) {
    throw AppError("bad_json", "请求体不是合法 JSON");
  }
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

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

const std::string& logo_jpeg() {
  static const std::string bytes = [] {
    const std::string hex = embedded_logo_jpg_hex();
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
      out.push_back(static_cast<char>((hex_digit(hex[i]) << 4) | hex_digit(hex[i + 1])));
    }
    return out;
  }();
  return bytes;
}

int status_for(const AppError& error) {
  const std::string& code = error.code();
  if (code == "duplicate" || code == "last_admin" || code == "self_delete" ||
      code == "no_user")
    return 409;
  if (code == "not_found") return 404;
  if (code == "legacy_db") return 409;
  if (code == "invalid_credentials") return 401;
  if (code == "locked") return 423;
  if (code == "forbidden" || code == "must_change_password") return 403;
  if (code == "invalid_device" || code == "invalid_user" ||
      code == "weak_password" || code == "bad_field" || code == "bad_json" ||
      code == "bad_header" || code == "bad_xlsx" || code == "not_legacy" ||
      code == "bad_backup" || code == "old_password_required")
    return 400;
  return 500;
}

std::string random_token() {
  static const char hex[] = "0123456789abcdef";
  std::random_device device;
  std::string token;
  token.reserve(48);
  for (int i = 0; i < 24; ++i) token.push_back(hex[device() % 16]);
  return token;
}

std::string cookie_value(const httplib::Request& request, const char* name) {
  const std::string header = request.get_header_value("Cookie");
  const std::string prefix = std::string(name) + "=";
  std::size_t start = 0;
  while (start < header.size()) {
    std::size_t end = header.find(';', start);
    if (end == std::string::npos) end = header.size();
    std::string part = header.substr(start, end - start);
    while (!part.empty() && std::isspace(static_cast<unsigned char>(part.front()))) {
      part.erase(part.begin());
    }
    if (part.rfind(prefix, 0) == 0) return part.substr(prefix.size());
    start = end + 1;
  }
  return {};
}

std::string session_user(ServerState& state, const httplib::Request& request) {
  const std::string token = cookie_value(request, "sid");
  if (token.empty()) return {};
  std::lock_guard<std::mutex> lock(state.sessions_mutex);
  auto it = state.sessions.find(token);
  if (it == state.sessions.end()) return {};
  const auto now = std::chrono::steady_clock::now();
  if (now - it->second.last_seen > kSessionTtl) {
    state.sessions.erase(it);
    return {};
  }
  it->second.last_seen = now;
  return it->second.user;
}

std::string create_session(ServerState& state, const std::string& user) {
  const std::string token = random_token();
  std::lock_guard<std::mutex> lock(state.sessions_mutex);
  state.sessions[token] = Session{user, std::chrono::steady_clock::now()};
  return token;
}

void destroy_session(ServerState& state, const httplib::Request& request) {
  const std::string token = cookie_value(request, "sid");
  if (token.empty()) return;
  std::lock_guard<std::mutex> lock(state.sessions_mutex);
  state.sessions.erase(token);
}

void set_session_cookie(httplib::Response& response, const std::string& token) {
  response.set_header("Set-Cookie",
                      "sid=" + token +
                          "; Path=/; HttpOnly; SameSite=Lax; Max-Age=43200");
}

void clear_session_cookie(httplib::Response& response) {
  response.set_header("Set-Cookie", "sid=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0");
}

bool is_loopback(const std::string& address) {
  return address == "127.0.0.1" || address == "::1" ||
         address == "::ffff:127.0.0.1";
}

bool host_allowed(std::string host) {
  if (host.empty()) return false;
  std::transform(host.begin(), host.end(), host.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::string name = host;
  if (!name.empty() && name.front() == '[') {
    const std::size_t end = name.find(']');
    if (end != std::string::npos) name = name.substr(1, end - 1);
  } else {
    const std::size_t colon = name.find(':');
    if (colon != std::string::npos) name = name.substr(0, colon);
  }
  return name == "127.0.0.1" || name == "localhost" || name == "::1";
}

User current_user(ServerState& state, const httplib::Request& request) {
  const std::string name = session_user(state, request);
  if (name.empty()) throw AppError("unauthorized", "未登录或会话已过期");
  return state.users->get(name);
}

User require_admin(ServerState& state, const httplib::Request& request) {
  User user = current_user(state, request);
  if (user.role != "admin") throw AppError("forbidden", "需要管理员权限");
  return user;
}

}  // namespace

struct ServerHandle::Impl : ServerState {};

ServerHandle::ServerHandle() : impl_(std::make_unique<Impl>()) {}
ServerHandle::~ServerHandle() {
  if (impl_ && impl_->server.is_running()) impl_->server.stop();
  if (impl_ && impl_->thread.joinable()) impl_->thread.join();
}

const std::string& ServerHandle::local_token() const { return impl_->local_token; }

std::unique_ptr<ServerHandle> start_server(const ServerOptions& options) {
  auto handle = std::unique_ptr<ServerHandle>(new ServerHandle());
  ServerState& state = *handle->impl_;
  state.data_dir = options.data_dir;

  std::error_code ec;
  std::filesystem::create_directories(options.data_dir, ec);
  if (ec) {
    throw AppError("data_dir", "无法创建数据目录: " + ec.message());
  }

  // 旧库自动探测：先备份、再只读导入，成功后旧库改名 *.migrated-<时间戳>。
  state.legacy = auto_import_if_needed(options.data_dir);

  state.db = std::make_unique<Db>(options.data_dir / "devicedb.db");
  migrate(*state.db);

  const SeedResult seed = seed_default_users(*state.db, options.data_dir);
  state.seeded = seed.seeded;
  if (seed.seeded) {
    std::cout << "首次启动：已生成初始账号，口令写入 " << seed.password_file
              << "（首次登录必须修改）" << std::endl;
    for (const auto& [name, password] : seed.accounts) {
      std::cout << "  用户名: " << name << "  初始口令: " << password << std::endl;
    }
  }

  state.devices = std::make_unique<DeviceService>(*state.db);
  state.users = std::make_unique<UserService>(*state.db);
  state.local_token = random_token();

  const auto schema = state.db->schema_version();
  const std::string version = DEVCARD_VERSION;

  state.server.Get("/api/health", [&state, schema, version](
                                     const httplib::Request&,
                                     httplib::Response& response) {
    set_json(response, json{{"status", "ok"},
                            {"version", version},
                            {"schema_version", schema},
                            {"mode", "local"},
                            {"legacy_imported", state.legacy.performed}});
  });

  state.server.Post("/api/session/local",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      if (!is_loopback(request.remote_addr)) {
                        throw AppError("forbidden", "本地会话只能从本机建立");
                      }
                      const json body = parse_body(request.body);
                      const std::string token = body.value("token", "");
                      if (token != state.local_token) {
                        throw AppError("invalid_credentials", "本地会话令牌无效");
                      }
                      const std::string name = state.users->local_user();
                      if (name.empty()) {
                        throw AppError("no_user",
                                       "没有可用账号，请执行 --set-password <用户名>");
                      }
                      const User user = state.users->get(name);
                      set_session_cookie(response, create_session(state, name));
                      set_json(response, json{{"user", user_json(user)}});
                    });

  state.server.Post("/api/session/login",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      const json body = parse_body(request.body);
                      const User user = state.users->authenticate(
                          body.value("username", ""), body.value("password", ""));
                      set_session_cookie(response,
                                         create_session(state, user.user_name));
                      set_json(response, json{{"user", user_json(user)}});
                    });

  state.server.Post("/api/session/logout",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      destroy_session(state, request);
                      clear_session_cookie(response);
                      set_json(response, json{{"ok", true}});
                    });

  state.server.Get("/api/bootstrap", [&state, schema, version](
                                         const httplib::Request& request,
                                         httplib::Response& response) {
    const User user = current_user(state, request);
    json legacy = nullptr;
    if (state.legacy.performed) {
      legacy = {{"source", state.legacy.source},
                {"backup", state.legacy.backup},
                {"archived_as", state.legacy.archived_as},
                {"devices_total", state.legacy.devices_total},
                {"devices_imported", state.legacy.devices_imported},
                {"devices_skipped", state.legacy.devices_skipped},
                {"users_total", state.legacy.users_total},
                {"users_imported", state.legacy.users_imported},
                {"users_skipped", state.legacy.users_skipped},
                {"warnings", state.legacy.warnings}};
    }
    set_json(response, json{{"user", user_json(user)},
                            {"version", version},
                            {"schema_version", schema},
                            {"data_dir", state.data_dir.string()},
                            {"legacy", legacy}});
  });

  state.server.Get("/api/devices", [&state](const httplib::Request& request,
                                            httplib::Response& response) {
    const DevicePage page = state.devices->list(query_from(request));
    json items = json::array();
    for (const Device& device : page.items) items.push_back(device_json(device));
    set_json(response, json{{"items", items},
                            {"total", page.total},
                            {"page", page.page},
                            {"page_size", page.page_size}});
  });

  state.server.Get("/api/devices/options",
                   [&state](const httplib::Request&, httplib::Response& response) {
                     const DeviceOptions values = state.devices->options();
                     set_json(response, json{{"names", values.names},
                                             {"locations", values.locations}});
                   });

  state.server.Post("/api/devices/import",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      const auto rows = xlsx::read(request.body);
                      const ImportReport report = state.devices->import_rows(rows);
                      set_json(response, json{{"imported", report.imported},
                                              {"skipped_empty", report.skipped_empty},
                                              {"duplicates", report.duplicates},
                                              {"truncated", report.truncated}});
                    });

  // 导入模板：第一个工作表只放表头（与导入校验同一份常量），
  // 第二个工作表放填写说明与假号码示例，避免模板与校验各写一份。
  state.server.Get("/api/devices/template",
                   [](const httplib::Request&, httplib::Response& response) {
                     xlsx::Sheet data;
                     data.name = "设备台账";
                     data.rows.push_back(DeviceService::expected_header());

                     xlsx::Sheet help;
                     help.name = "填写说明";
                     help.rows = {
                         {"列名", "含义与要求"},
                         {"设备名称", "必填；导入时超过 10 个字符会被截断"},
                         {"位置", "必填；导入时超过 10 个字符会被截断"},
                         {"控制范围", "必填；导入时超过 10 个字符会被截断"},
                         {"维护电话", "必填；仅数字、空格、短横线；导入时超过 11 个字符会被截断"},
                         {},
                         {"示例（假号码，请勿直接使用）",
                          "13800000001（卡片上显示为 138 0000 0001）"},
                         {},
                         {"说明",
                          "第一个工作表只有表头，可直接上传，导入 0 行；四列全空的行会跳过，"
                          "重复记录照常导入并提示。"},
                     };

                     response.status = 200;
                     response.set_header(
                         "Content-Type",
                         "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet");
                     response.set_header(
                         "Content-Disposition",
                         "attachment; filename=\"devices-template.xlsx\"; "
                         "filename*=UTF-8''%E8%AE%BE%E5%A4%87%E5%AF%BC%E5%85%A5"
                         "%E6%A8%A1%E6%9D%BF.xlsx");
                     response.set_header("Cache-Control", "no-store");
                     response.body = xlsx::write_workbook({data, help});
                   });

  state.server.Get("/api/devices/export",
                   [&state](const httplib::Request&, httplib::Response& response) {
                     const std::string body =
                         xlsx::write(state.devices->export_rows());
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

  // 打印数据快照：与台账共用筛选条件，一次取全量（不分页），供前端排版。
  // 前端导出 PDF 与直接打印复用同一份返回，不再二次请求。
  state.server.Get("/api/print/cards",
                   [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     DeviceQuery query = query_from(request);
                     query.page = 0;
                     query.page_size = 1000;
                     query.sort = "id";
                     query.desc = false;

                     std::vector<Device> items;
                     std::int64_t total = 0;
                     bool truncated = false;
                     while (true) {
                       const DevicePage page = state.devices->list(query);
                       if (query.page == 0) total = page.total;
                       items.insert(items.end(), page.items.begin(), page.items.end());
                       if (page.items.empty() ||
                           static_cast<std::int64_t>(items.size()) >= total) {
                         break;
                       }
                       if (items.size() >= 10000) {
                         truncated = true;
                         break;
                       }
                       ++query.page;
                     }

                     const std::string ids_text = request.get_param_value("ids");
                     if (!ids_text.empty()) {
                       std::set<std::int64_t> wanted;
                       std::size_t start = 0;
                       while (start <= ids_text.size()) {
                         const std::size_t comma = ids_text.find(',', start);
                         const std::string part = ids_text.substr(
                             start, comma == std::string::npos
                                        ? std::string::npos
                                        : comma - start);
                         try {
                           if (!part.empty()) wanted.insert(std::stoll(part));
                         } catch (...) {
                           throw AppError("bad_json", "ids 必须是逗号分隔的数字");
                         }
                         if (comma == std::string::npos) break;
                         start = comma + 1;
                       }
                       items.erase(
                           std::remove_if(items.begin(), items.end(),
                                          [&](const Device& device) {
                                            return wanted.count(device.id) == 0;
                                          }),
                           items.end());
                       total = static_cast<std::int64_t>(items.size());
                     }

                     json array = json::array();
                     for (const Device& device : items) {
                       array.push_back(device_json(device));
                     }
                     set_json(response, json{{"items", array},
                                             {"total", total},
                                             {"truncated", truncated}});
                   });

  state.server.Get(R"(/api/devices/(\d+))",
                   [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     const auto id = std::stoll(request.matches[1].str());
                     set_json(response, device_json(state.devices->get(id)));
                   });

  state.server.Post("/api/devices", [&state](const httplib::Request& request,
                                             httplib::Response& response) {
    Device device = device_from_json(parse_body(request.body));
    set_json(response, device_json(state.devices->create(std::move(device))), 201);
  });

  state.server.Put(R"(/api/devices/(\d+))",
                   [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     const auto id = std::stoll(request.matches[1].str());
                     Device device = device_from_json(parse_body(request.body));
                     set_json(response,
                              device_json(state.devices->update(id, std::move(device))));
                   });

  state.server.Delete(R"(/api/devices/(\d+))",
                      [&state](const httplib::Request& request,
                               httplib::Response& response) {
                        state.devices->remove(std::stoll(request.matches[1].str()));
                        set_json(response, json{{"ok", true}});
                      });

  state.server.Get("/api/users", [&state](const httplib::Request& request,
                                          httplib::Response& response) {
    require_admin(state, request);
    json items = json::array();
    for (const User& user : state.users->list()) items.push_back(user_json(user));
    set_json(response, json{{"items", items}});
  });

  state.server.Post("/api/users", [&state](const httplib::Request& request,
                                           httplib::Response& response) {
    require_admin(state, request);
    const json body = parse_body(request.body);
    const User user = state.users->create(
        body.value("user_name", ""), body.value("password", ""),
        body.value("display_name", ""), body.value("role", "user"));
    set_json(response, json{{"user", user_json(user)}}, 201);
  });

  state.server.Put("/api/users", [&state](const httplib::Request& request,
                                          httplib::Response& response) {
    require_admin(state, request);
    const json body = parse_body(request.body);
    const User user = state.users->update_profile(
        body.value("user_name", ""), body.value("display_name", ""),
        body.value("role", "user"));
    set_json(response, json{{"user", user_json(user)}});
  });

  state.server.Delete("/api/users", [&state](const httplib::Request& request,
                                             httplib::Response& response) {
    const User actor = require_admin(state, request);
    const std::string name = request.get_param_value("name");
    state.users->remove(name, actor.user_name);
    set_json(response, json{{"ok", true}});
  });

  state.server.Post("/api/users/password",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      const User actor = current_user(state, request);
                      const json body = parse_body(request.body);
                      std::string target = body.value("user_name", "");
                      if (target.empty()) target = actor.user_name;
                      const std::string new_password = body.value("new_password", "");
                      if (new_password.empty()) {
                        throw AppError("bad_json", "new_password 不能为空");
                      }
                      if (target == actor.user_name) {
                        const std::string old_password =
                            body.value("old_password", "");
                        if (old_password.empty()) {
                          throw AppError("old_password_required", "请输入原口令");
                        }
                        state.users->change_password(actor.user_name, old_password,
                                                     new_password);
                      } else {
                        if (actor.role != "admin") {
                          throw AppError("forbidden", "需要管理员权限");
                        }
                        state.users->reset_password(target, new_password);
                      }
                      if (!state.users->any_must_change()) {
                        remove_seed_password_file(state.data_dir);
                      }
                      set_json(response, json{{"ok", true}, {"user_name", target}});
                    });

  state.server.Get("/api/system/backups",
                   [&state](const httplib::Request& request,
                            httplib::Response& response) {
                     require_admin(state, request);
                     json items = json::array();
                     for (const BackupEntry& entry : list_backups(state.data_dir)) {
                       items.push_back(json{{"name", entry.name},
                                            {"bytes", entry.bytes},
                                            {"created_at", entry.created_at}});
                     }
                     set_json(response, json{{"items", items}});
                   });

  state.server.Post("/api/system/backup",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      require_admin(state, request);
                      const BackupEntry entry =
                          create_backup(*state.db, state.data_dir, "manual");
                      set_json(response, json{{"name", entry.name},
                                              {"bytes", entry.bytes},
                                              {"created_at", entry.created_at}}, 201);
                    });

  state.server.Post("/api/system/restore",
                    [&state](const httplib::Request& request,
                             httplib::Response& response) {
                      require_admin(state, request);
                      const json body = parse_body(request.body);
                      if (body.value("confirm", "") != "RESTORE") {
                        throw AppError("bad_json", "恢复操作需要 confirm=RESTORE");
                      }
                      restore_backup(*state.db, state.data_dir,
                                     body.value("name", ""));
                      {
                        std::lock_guard<std::mutex> lock(state.sessions_mutex);
                        state.sessions.clear();
                      }
                      clear_session_cookie(response);
                      set_json(response, json{{"ok", true}, {"reload", true}});
                    });

  state.server.Get("/", [&state](const httplib::Request&, httplib::Response& response) {
    std::string html = embedded_index_html();
    const std::string meta = "<meta name=\"devcard-local-token\" content=\"" +
                             state.local_token + "\">";
    const std::size_t head = html.find("</head>");
    if (head != std::string::npos) html.insert(head, meta + "\n");
    set_asset(response, html, "text/html; charset=utf-8");
  });
  state.server.Get("/styles.css", [](const httplib::Request&, httplib::Response& response) {
    set_asset(response, embedded_styles_css(), "text/css; charset=utf-8");
  });
  state.server.Get("/app.js", [](const httplib::Request&, httplib::Response& response) {
    set_asset(response, embedded_app_js(), "application/javascript; charset=utf-8");
  });
  state.server.Get("/logo.jpg", [](const httplib::Request&, httplib::Response& response) {
    response.status = 200;
    response.set_header("Content-Type", "image/jpeg");
    response.set_header("Cache-Control", "public, max-age=86400");
    response.body = logo_jpeg();
  });

  // 认证与 CSRF 兜底：/api/health 与 /api/session/* 免认证。
  state.server.set_pre_routing_handler(
      [&state](const httplib::Request& request, httplib::Response& response) {
        if (!host_allowed(request.get_header_value("Host"))) {
          set_json(response,
                   json{{"error", "bad_host"},
                        {"message", "Host 头必须是 127.0.0.1 或 localhost"}},
                   421);
          return httplib::Server::HandlerResponse::Handled;
        }
        if (request.path.rfind("/api/", 0) != 0) {
          return httplib::Server::HandlerResponse::Unhandled;
        }
        if (request.path == "/api/health" ||
            request.path.rfind("/api/session/", 0) == 0) {
          return httplib::Server::HandlerResponse::Unhandled;
        }
        if (request.method != "GET" &&
            request.get_header_value("X-Requested-With") != "DevCard") {
          set_json(response,
                   json{{"error", "csrf"},
                        {"message", "缺少 X-Requested-With: DevCard"}},
                   403);
          return httplib::Server::HandlerResponse::Handled;
        }
        const std::string name = session_user(state, request);
        if (name.empty()) {
          set_json(response,
                   json{{"error", "unauthorized"}, {"message", "未登录或会话已过期"}},
                   401);
          return httplib::Server::HandlerResponse::Handled;
        }
        User user;
        try {
          user = state.users->get(name);
        } catch (const AppError&) {
          set_json(response,
                   json{{"error", "unauthorized"}, {"message", "账号不存在"}}, 401);
          return httplib::Server::HandlerResponse::Handled;
        }
        const bool password_paths =
            request.path == "/api/bootstrap" ||
            request.path == "/api/users/password";
        if (user.must_change_password && !password_paths) {
          set_json(response,
                   json{{"error", "must_change_password"},
                        {"message", "首次登录必须修改口令"}},
                   403);
          return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
      });

  state.server.set_post_routing_handler(
      [](const httplib::Request&, httplib::Response& response) {
        response.set_header("X-Content-Type-Options", "nosniff");
        response.set_header("Referrer-Policy", "no-referrer");
        if (response.get_header_value("Content-Type").rfind("text/html", 0) == 0) {
          response.set_header(
              "Content-Security-Policy",
              "default-src 'self'; img-src 'self' data:; style-src 'self'");
        }
      });

  state.server.set_error_handler(
      [](const httplib::Request&, httplib::Response& response) {
        if (response.status == 404 && response.body.empty()) {
          set_json(response, json{{"error", "not_found"}, {"message", "接口不存在"}},
                   404);
        }
      });

  state.server.set_exception_handler([](const httplib::Request&,
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

  if (!state.server.bind_to_port(options.host, options.port)) {
    throw AppError("port_in_use",
                   "无法监听 " + options.host + ":" + std::to_string(options.port));
  }
  handle->port_ = options.port;
  state.thread = std::thread([&state] { state.server.listen_after_bind(); });
  return handle;
}

}  // namespace devcard
